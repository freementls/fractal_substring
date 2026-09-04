/*
 * fss_repeats.c — repeated-substring discovery (all_substrings_count successor).
 *
 * Fast path: hash fixed windows → duplicate pairs → extend by memcmp →
 * one fss_count verify. Cap verify budget so wall stays well under ASC slide.
 */
#include "fss_internal.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef fss_cand cand;

static int marker_len_heuristic(size_t len, int base) {
	if (base > 0) return base;
	size_t m = 4 + (len >= 10 ? 2 : 0) + (len >= 100 ? 1 : 0);
	return (int)m;
}

static int est_lin(size_t len, size_t n, uint32_t count, int mlen) {
	long long v = (long long)len + (long long)n -
	              (long long)count * ((long long)len - mlen);
	if (v > 2147483647LL) return 2147483647;
	if (v < -2147483647LL) return -2147483647;
	return (int)v;
}

static int cand_cmp(const void *a, const void *b) {
	const cand *x = (const cand *)a;
	const cand *y = (const cand *)b;
	if (x->est_lin != y->est_lin) return x->est_lin < y->est_lin ? -1 : 1;
	if (x->len != y->len) return x->len > y->len ? -1 : 1;
	if (x->count != y->count) return x->count > y->count ? -1 : 1;
	return 0;
}

typedef struct {
	uint64_t h;
	size_t off;
} hash_off;

static int hash_off_cmp(const void *a, const void *b) {
	const hash_off *x = (const hash_off *)a;
	const hash_off *y = (const hash_off *)b;
	if (x->h < y->h) return -1;
	if (x->h > y->h) return 1;
	if (x->off < y->off) return -1;
	if (x->off > y->off) return 1;
	return 0;
}

static void extend_pair(const uint8_t *h, size_t n, size_t a, size_t b,
                        size_t seed_len, size_t min_len, size_t max_len,
                        size_t *out_off, size_t *out_len) {
	size_t L = 0;
	while (a >= L + 1 && b >= L + 1 && h[a - L - 1] == h[b - L - 1]) L++;
	size_t R = seed_len;
	while (a + R < n && b + R < n && h[a + R] == h[b + R]) R++;
	size_t len = L + R;
	size_t off = a - L;
	if (len > max_len) len = max_len;
	if (len < min_len) {
		*out_len = 0;
		return;
	}
	*out_off = off;
	*out_len = len;
}

static int already_have(const cand *c, size_t nc, const uint8_t *h, size_t off,
                        size_t len) {
	for (size_t i = 0; i < nc; i++) {
		if (c[i].len == len && memcmp(h + c[i].off, h + off, len) == 0)
			return 1;
	}
	return 0;
}

static size_t harvest_chunk_dups(const uint8_t *h, size_t n, size_t chunk,
                                 size_t stride, size_t scan_off, size_t scan_n,
                                 size_t min_len, size_t max_len,
                                 uint32_t min_count, int marker_base,
                                 const fss_profile *prof, cand *cands,
                                 size_t max_cands, size_t nc,
                                 size_t verify_budget, size_t *verifies) {
	if (chunk > max_len || chunk < min_len || scan_n < chunk) return nc;
	size_t nwin = 1 + (scan_n - chunk) / stride;
	if (nwin > 100000) {
		stride = (scan_n - chunk) / 100000 + 1;
		nwin = 1 + (scan_n - chunk) / stride;
	}
	hash_off *wins = (hash_off *)malloc(nwin * sizeof(hash_off));
	if (!wins) return nc;

	size_t w = 0;
	for (size_t off = scan_off;
	     off + chunk <= scan_off + scan_n && off + chunk <= n; off += stride) {
		wins[w].h = fss_hash_bytes(h + off, chunk);
		wins[w].off = off;
		w++;
		if (w >= nwin) break;
	}
	nwin = w;
	qsort(wins, nwin, sizeof(hash_off), hash_off_cmp);

	for (size_t i = 0; i < nwin && nc < max_cands;) {
		size_t j = i + 1;
		while (j < nwin && wins[j].h == wins[i].h) j++;
		if (j - i >= 2 && *verifies < verify_budget) {
			size_t a = wins[i].off;
			size_t b = wins[i + 1].off;
			if (memcmp(h + a, h + b, chunk) == 0) {
				size_t eoff = 0, elen = 0;
				extend_pair(h, n, a, b, chunk, min_len, max_len, &eoff, &elen);
				if (elen >= min_len && !already_have(cands, nc, h, eoff, elen)) {
					(*verifies)++;
					uint32_t cnt = (uint32_t)fss_count_ex(h, n, h + eoff, elen,
					                                      0, prof);
					if (cnt >= min_count) {
						int ml = marker_len_heuristic(elen, marker_base);
						cands[nc].off = eoff;
						cands[nc].len = elen;
						cands[nc].count = cnt;
						cands[nc].est_lin = est_lin(elen, n, cnt, ml);
						nc++;
						/* One ASC-like short prefix per long hit (PHP re-verifies). */
						size_t short_len = min_len < 5 ? 5 : min_len;
						if (short_len > 8) short_len = 8;
						if (short_len < elen && nc < max_cands &&
						    *verifies < verify_budget &&
						    !already_have(cands, nc, h, eoff, short_len)) {
							(*verifies)++;
							uint32_t sc = (uint32_t)fss_count_ex(
							    h, n, h + eoff, short_len, 0, prof);
							if (sc >= min_count) {
								int sml =
								    marker_len_heuristic(short_len, marker_base);
								cands[nc].off = eoff;
								cands[nc].len = short_len;
								cands[nc].count = sc;
								cands[nc].est_lin =
								    est_lin(short_len, n, sc, sml);
								nc++;
							}
						}
					}
				}
			}
		}
		i = j;
	}
	free(wins);
	return nc;
}

/* Gear table for content-defined chunking (fmem-compatible avg ~4 KiB). */
static uint64_t g_gear[256];
static int g_gear_ready;

static void ensure_gear(void) {
	if (g_gear_ready) return;
	uint64_t seed = 0x9e3779b97f4a7c15ULL;
	for (int i = 0; i < 256; i++) {
		seed ^= seed >> 12;
		seed ^= seed << 25;
		seed ^= seed >> 27;
		seed *= 0x2545F4914F6CDD1DULL;
		g_gear[i] = seed + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
	}
	g_gear_ready = 1;
}

typedef struct {
	uint64_t h;
	size_t off;
	size_t len;
} cdc_chunk;

static int cdc_cmp(const void *a, const void *b) {
	const cdc_chunk *x = (const cdc_chunk *)a;
	const cdc_chunk *y = (const cdc_chunk *)b;
	if (x->h < y->h) return -1;
	if (x->h > y->h) return 1;
	if (x->off < y->off) return -1;
	if (x->off > y->off) return 1;
	return 0;
}

/* Cross-block free seeds from CDC twin chunks (no full-n SA). */
static size_t harvest_cdc_twins(const uint8_t *h, size_t n, size_t min_len,
                                size_t max_len, uint32_t min_count,
                                int marker_base, const fss_profile *prof,
                                cand *cands, size_t max_cands, size_t nc,
                                size_t verify_budget, size_t *verifies) {
	ensure_gear();
	enum { CDC_MIN = 1024, CDC_MAX = 16384, CDC_MASK = 0xFFF };
	size_t est = n / (CDC_MASK + 1) + 8;
	if (est > 500000) est = 500000;
	cdc_chunk *ch = (cdc_chunk *)malloc(est * sizeof(cdc_chunk));
	if (!ch) return nc;

	size_t cnt = 0;
	size_t beg = 0;
	uint64_t gh = 0;
	for (size_t pos = 0; pos <= n && cnt < est;) {
		size_t len = pos - beg;
		int cut = (pos == n) ||
		          (len >= CDC_MIN && (gh & CDC_MASK) == 0) ||
		          len >= CDC_MAX;
		if (cut && len > 0) {
			ch[cnt].off = beg;
			ch[cnt].len = len;
			ch[cnt].h = fss_hash_bytes(h + beg, len);
			cnt++;
			beg = pos;
			gh = 0;
			if (pos == n) break;
			continue;
		}
		if (pos < n) gh = (gh << 1) + g_gear[h[pos]];
		pos++;
	}

	qsort(ch, cnt, sizeof(cdc_chunk), cdc_cmp);
	for (size_t i = 0; i < cnt && nc < max_cands;) {
		size_t j = i + 1;
		while (j < cnt && ch[j].h == ch[i].h) j++;
		if (j - i >= 2 && *verifies < verify_budget) {
			size_t a = ch[i].off;
			size_t b = ch[i + 1].off;
			size_t clen = ch[i].len;
			if (clen == ch[i + 1].len && clen >= min_len &&
			    memcmp(h + a, h + b, clen) == 0) {
				size_t eoff = 0, elen = 0;
				size_t seed = clen > max_len ? max_len : clen;
				extend_pair(h, n, a, b, seed, min_len, max_len, &eoff, &elen);
				if (elen >= min_len && !already_have(cands, nc, h, eoff, elen)) {
					(*verifies)++;
					uint32_t ccnt = (uint32_t)fss_count_ex(h, n, h + eoff, elen,
					                                       0, prof);
					if (ccnt >= min_count) {
						int ml = marker_len_heuristic(elen, marker_base);
						cands[nc].off = eoff;
						cands[nc].len = elen;
						cands[nc].count = ccnt;
						cands[nc].est_lin = est_lin(elen, n, ccnt, ml);
						nc++;
					}
				}
			}
		}
		i = j;
	}
	free(ch);
	return nc;
}

/* Fixed-window twins (complements CDC when gear boundaries don't realign). */
static size_t harvest_fixed_twins(const uint8_t *h, size_t n, size_t chunk,
                                  size_t min_len, size_t max_len,
                                  uint32_t min_count, int marker_base,
                                  const fss_profile *prof, cand *cands,
                                  size_t max_cands, size_t nc,
                                  size_t verify_budget, size_t *verifies) {
	if (chunk < min_len || chunk > max_len || n < chunk * 2) return nc;
	return harvest_chunk_dups(h, n, chunk, chunk, 0, n, min_len, max_len,
	                          min_count, marker_base, prof, cands, max_cands, nc,
	                          verify_budget, verifies);
}

/*
 * Rare 3-gram landmarks: sample trigrams, keep those with 2..64 hits (not
 * boilerplate spaces), extend pairs — plan "saccades" without a full SA.
 */
static size_t harvest_rare_grams(const uint8_t *h, size_t n, size_t scan_off,
                                 size_t scan_n, size_t min_len, size_t max_len,
                                 uint32_t min_count, int marker_base,
                                 const fss_profile *prof, cand *cands,
                                 size_t max_cands, size_t nc,
                                 size_t verify_budget, size_t *verifies) {
	if (scan_n < 3 || nc >= max_cands) return nc;
	size_t stride = scan_n > 64 * 1024 ? 8 : (scan_n > 16384 ? 4 : 2);
	size_t est = 1 + scan_n / stride;
	if (est > 200000) {
		stride = scan_n / 200000 + 1;
		est = 1 + scan_n / stride;
	}
	hash_off *wins = (hash_off *)malloc(est * sizeof(hash_off));
	if (!wins) return nc;
	size_t w = 0;
	size_t end = scan_off + scan_n;
	if (end > n) end = n;
	for (size_t off = scan_off; off + 3 <= end && w < est; off += stride) {
		uint64_t g = ((uint64_t)h[off] << 16) | ((uint64_t)h[off + 1] << 8) |
		             (uint64_t)h[off + 2];
		/* Skip ultra-common whitespace/zero runs. */
		if (h[off] == h[off + 1] && h[off + 1] == h[off + 2] &&
		    (h[off] == ' ' || h[off] == '\0' || h[off] == '\n'))
			continue;
		wins[w].h = g;
		wins[w].off = off;
		w++;
	}
	qsort(wins, w, sizeof(hash_off), hash_off_cmp);

	size_t added = 0;
	for (size_t i = 0; i < w && nc < max_cands && added < 32;) {
		size_t j = i + 1;
		while (j < w && wins[j].h == wins[i].h) j++;
		size_t hits = j - i;
		/* Rare-but-repeated: not unique, not ubiquitous. */
		if (hits >= 2 && hits <= 64 && *verifies < verify_budget) {
			size_t a = wins[i].off;
			size_t b = wins[i + 1].off;
			size_t eoff = 0, elen = 0;
			extend_pair(h, n, a, b, 3, min_len, max_len, &eoff, &elen);
			if (elen >= min_len && !already_have(cands, nc, h, eoff, elen)) {
				(*verifies)++;
				uint32_t cnt = (uint32_t)fss_count_ex(h, n, h + eoff, elen, 0,
				                                      prof);
				if (cnt >= min_count) {
					int ml = marker_len_heuristic(elen, marker_base);
					cands[nc].off = eoff;
					cands[nc].len = elen;
					cands[nc].count = cnt;
					cands[nc].est_lin = est_lin(elen, n, cnt, ml);
					nc++;
					added++;
				}
			}
		}
		i = j;
	}
	free(wins);
	return nc;
}

static int block_entropy_label(const uint8_t *h, size_t n, size_t off,
                               size_t len) {
	if (off >= n) return 0;
	if (off + len > n) len = n - off;
	size_t sample = len > 4096 ? 4096 : len;
	uint32_t hist[256];
	memset(hist, 0, sizeof(hist));
	for (size_t i = 0; i < sample; i++) hist[h[off + i]]++;
	double H = 0.0;
	double inv = 1.0 / (double)(sample ? sample : 1);
	for (int i = 0; i < 256; i++) {
		if (!hist[i]) continue;
		double p = (double)hist[i] * inv;
		H -= p * log2(p);
	}
	if (H <= 3.5) return 2;
	if (H <= 5.5) return 1;
	return 0;
}

size_t fss_repeats_ex(const uint8_t *h, size_t n, const fss_repeat_opts *o,
                      fss_repeat *out, size_t out_cap,
                      const fss_profile *prof) {
	if (!h || !o || !out || out_cap == 0 || n < o->min_len * 2) return 0;

	size_t min_len = o->min_len ? o->min_len : 4;
	size_t max_len = o->max_len ? o->max_len : 256;
	if (max_len > n / 2) max_len = n / 2;
	uint32_t min_count = o->min_count ? o->min_count : 2;
	size_t top_k = o->top_k ? o->top_k : 24;
	if (top_k > out_cap) top_k = out_cap;

	const char *label = fss_profile_label(prof);
	int is_high = (label && label[0] == 'h');
	int is_low = (label && label[0] == 'l');

	size_t max_cands = 4096;
	if (n >= 192 * 1024) {
		max_cands = is_low ? 1024 : 2048;
		if (top_k > 16) top_k = 16;
	}
	if (n >= (size_t)1 << 24) {
		max_cands = is_high ? 2048 : 512;
		if (top_k > 12) top_k = 12;
	}

	cand *cands = (cand *)calloc(max_cands, sizeof(cand));
	if (!cands) return 0;
	size_t nc = 0;
	size_t verifies = 0;
	/* Budget: keep repeats well under ASC slide cost. */
	size_t verify_budget = 64 + (n / 8192);
	if (verify_budget > 256) verify_budget = 256;

	/* ≥128 KiB: CDC/fixed first; strong long twins skip dense (langref bitwise
	 * ~180 KiB was still on the medium stride path at 192 KiB gate). */
	int immense = (n >= 128 * 1024);

	if (immense) {
		/* 1) CDC twins — free cross-block exact repeats. */
		nc = harvest_cdc_twins(h, n, min_len, max_len, min_count,
		                       o->marker_len_base, prof, cands, max_cands, nc,
		                       verify_budget, &verifies);

		/* Fixed 4 KiB / 1 KiB windows catch planted twins CDC can miss. */
		if (nc < top_k && verifies < verify_budget) {
			size_t fx = 4096;
			if (fx >= min_len && fx <= max_len)
				nc = harvest_fixed_twins(h, n, fx, min_len, max_len, min_count,
				                         o->marker_len_base, prof, cands,
				                         max_cands, nc, verify_budget, &verifies);
			fx = 1024;
			if (nc < top_k && fx >= min_len && fx <= max_len)
				nc = harvest_fixed_twins(h, n, fx, min_len, max_len, min_count,
				                         o->marker_len_base, prof, cands,
				                         max_cands, nc, verify_budget, &verifies);
		}

		/*
		 * Strong long twins already found: deriving short ASC seeds from them
		 * is enough. Dense/SA/rare on periodic immense inputs burn the verify
		 * budget on near-identical windows (~400 ms/MiB).
		 */
		int strong_long = 0;
		for (size_t i = 0; i < nc; i++) {
			if (cands[i].len >= 64 && cands[i].count >= min_count) {
				strong_long = 1;
				break;
			}
		}
		if (strong_long) {
			size_t short_len = min_len < 5 ? 5 : min_len;
			if (short_len > 8) short_len = 8;
			for (size_t i = 0; i < nc && nc < top_k && verifies < verify_budget;
			     i++) {
				if (cands[i].len <= short_len) continue;
				size_t eoff = cands[i].off;
				if (already_have(cands, nc, h, eoff, short_len)) continue;
				verifies++;
				uint32_t sc = (uint32_t)fss_count_ex(h, n, h + eoff, short_len,
				                                     0, prof);
				if (sc < min_count) continue;
				int sml = marker_len_heuristic(short_len, o->marker_len_base);
				cands[nc].off = eoff;
				cands[nc].len = short_len;
				cands[nc].count = sc;
				cands[nc].est_lin = est_lin(short_len, n, sc, sml);
				nc++;
			}
		} else if (nc < top_k) {
			/* 2) Block-local harvest only while still hungry for diversity. */
			size_t block = 65536;
			size_t nblocks = (n + block - 1) / block;
			size_t dense_budget = is_low ? 4 : (is_high ? 12 : 8);
			if (n >= (size_t)1 << 22) dense_budget = is_high ? 8 : 4;
			size_t dense_done = 0;
			size_t scales_loc[] = {16, 32, 64};
			for (size_t bi = 0; bi < nblocks && nc < top_k &&
			                    dense_done < dense_budget &&
			                    verifies < verify_budget; bi++) {
				size_t off = bi * block;
				size_t len = block;
				if (off + len > n) len = n - off;
				if (len < min_len * 2) continue;
				int bl = block_entropy_label(h, n, off, len);
				if (bl == 0 && !is_high) continue;
				if (bl == 0 && is_high && (bi & 3) != 0) continue;
				dense_done++;
				for (size_t si = 0; si < 3 && nc < top_k; si++) {
					size_t chunk = scales_loc[si];
					if (chunk < min_len || chunk > max_len) continue;
					size_t stride = chunk;
					nc = harvest_chunk_dups(h, n, chunk, stride, off, len, min_len,
					                        max_len, min_count, o->marker_len_base,
					                        prof, cands, max_cands, nc,
					                        verify_budget, &verifies);
				}
			}

			/* L4: up to a few high blocks get SA+LCP (never full-n). */
			if (is_high && nc < top_k && verifies < verify_budget) {
				size_t sab = 32768;
				size_t nsab = (n + sab - 1) / sab;
				size_t sa_done = 0;
				for (size_t bi = 0; bi < nsab && sa_done < 3 && nc < top_k; bi++) {
					size_t off = bi * sab;
					size_t len = sab;
					if (off + len > n) len = n - off;
					if (len < 1024) continue;
					if (block_entropy_label(h, n, off, len) < 2) continue;
					sa_done++;
					nc = fss_harvest_sa_block(h, n, off, len, min_len, max_len,
					                          min_count, o->marker_len_base, prof,
					                          cands, max_cands, nc, verify_budget,
					                          &verifies);
				}
			}

			/* Rare-gram saccades when still hungry. */
			if (!is_low && nc < top_k && verifies < verify_budget) {
				size_t sn = n > (1u << 22) ? (1u << 22) : n;
				nc = harvest_rare_grams(h, n, 0, sn, min_len, max_len, min_count,
				                        o->marker_len_base, prof, cands, max_cands,
				                        nc, verify_budget, &verifies);
			}
		}
	} else {
		size_t scan_n = n;
		size_t scan_off = 0;

		/* L4: whole-string SA+LCP on medium high-redundancy inputs. */
		if (is_high && n <= 65536 && n >= 256) {
			nc = fss_harvest_sa_block(h, n, 0, n, min_len, max_len, min_count,
			                          o->marker_len_base, prof, cands, max_cands,
			                          nc, verify_budget, &verifies);
		}

		/* Multi-scale chunk harvest: include min_len up through 128. */
		size_t scales[8];
		size_t nscales = 0;
		scales[nscales++] = min_len;
		if (min_len < 8) scales[nscales++] = 8;
		scales[nscales++] = 16;
		scales[nscales++] = 32;
		scales[nscales++] = 64;
		scales[nscales++] = 128;
		for (size_t si = 0; si < nscales && nc < top_k * 2; si++) {
			size_t chunk = scales[si];
			if (chunk < min_len || chunk > max_len) continue;
			int seen = 0;
			for (size_t k = 0; k < si; k++)
				if (scales[k] == chunk) seen = 1;
			if (seen) continue;
			/*
			 * stride=1 on chunk≤8 over a 100 KiB scan is ~17 ms of hash+qsort
			 * per scale (langref). Use chunk stride once the window is large;
			 * short-seed / rare-gram passes still catch ASC-length tokens.
			 */
			size_t stride;
			if (chunk <= 8)
				stride = (scan_n >= 16384) ? chunk : 1;
			else if (!is_high)
				stride = chunk;
			else
				stride = chunk / 2;
			nc = harvest_chunk_dups(h, n, chunk, stride, scan_off, scan_n, min_len,
			                        max_len, min_count, o->marker_len_base, prof,
			                        cands, max_cands, nc, verify_budget, &verifies);
		}

		/* Cheap short-seed pass: fill ASC-like tokens when slide will be skipped. */
		if (!is_low && nc < top_k * 2 && verifies < verify_budget) {
			size_t glen = min_len < 8 ? min_len : 8;
			size_t step = glen * 8;
			size_t limit = scan_off + scan_n < n ? scan_off + scan_n : n;
			size_t probes = 0;
			for (size_t off = scan_off;
			     off + glen <= limit && nc < max_cands && probes < 64; off += step) {
				probes++;
				ssize_t next = fss_find_ex(h + off + 1, n - off - 1, h + off, glen,
				                           prof);
				if (next < 0) continue;
				size_t b = off + 1 + (size_t)next;
				size_t eoff = 0, elen = 0;
				extend_pair(h, n, off, b, glen, min_len, max_len, &eoff, &elen);
				if (elen < min_len || already_have(cands, nc, h, eoff, elen))
					continue;
				if (verifies >= verify_budget) break;
				verifies++;
				uint32_t cnt =
				    (uint32_t)fss_count_ex(h, n, h + eoff, elen, 0, prof);
				if (cnt < min_count) continue;
				int ml = marker_len_heuristic(elen, o->marker_len_base);
				cands[nc].off = eoff;
				cands[nc].len = elen;
				cands[nc].count = cnt;
				cands[nc].est_lin = est_lin(elen, n, cnt, ml);
				nc++;
			}
		}

		if (!is_low && nc < top_k && verifies < verify_budget) {
			nc = harvest_rare_grams(h, n, scan_off, scan_n, min_len, max_len,
			                        min_count, o->marker_len_base, prof, cands,
			                        max_cands, nc, verify_budget, &verifies);
		}
	}

	if (nc == 0) {
		free(cands);
		return 0;
	}

	qsort(cands, nc, sizeof(cand), cand_cmp);

	size_t written = 0;
	for (size_t i = 0; i < nc && written < top_k; i++) {
		int dup = 0;
		for (size_t j = 0; j < written; j++) {
			if (out[j].len == cands[i].len &&
			    memcmp(out[j].p, h + cands[i].off, cands[i].len) == 0) {
				dup = 1;
				break;
			}
		}
		if (dup) continue;
		out[written].p = h + cands[i].off;
		out[written].len = cands[i].len;
		out[written].count = cands[i].count;
		out[written].est_lin = cands[i].est_lin;
		written++;
	}
	free(cands);
	return written;
}

size_t fss_repeats(const uint8_t *h, size_t n, const fss_repeat_opts *o,
                   fss_repeat *out, size_t out_cap) {
	int budget = (n >= 4096) ? 2 : 1;
	fss_profile *prof = fss_profile_build(h, n, budget);
	size_t r = fss_repeats_ex(h, n, o, out, out_cap, prof);
	fss_profile_free(prof);
	return r;
}
