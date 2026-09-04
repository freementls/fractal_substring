/*
 * fss_find.c — exact substring search.
 *
 * Dispatcher (Pareto):
 *   m==0                 -> 0
 *   n < 64               -> glibc memmem (never lose on tiny)
 *   m==1                 -> memchr
 *   m==2..3              -> memchr dual/triple (beats AVX2 when nd[0] common)
 *   n>=64 + AVX2 + m>=4  -> AVX2 dual first+last prefilter + memcmp
 *                          (uniform needles -> Horspool; first-only lost on 'aaaa')
 *   else                 -> Two-Way (linear worst case)
 *   Horspool / memchr walk for count
 */
#include "fss_internal.h"

#include <string.h>

#if defined(__x86_64__) || defined(_M_X64)
#  include <cpuid.h>
#  include <immintrin.h>
#  define FSS_HAVE_X86 1
#endif

static ssize_t find_byte(const uint8_t *h, size_t n, uint8_t c) {
	const void *p = memchr(h, c, n);
	return p ? (ssize_t)((const uint8_t *)p - h) : (ssize_t)-1;
}

static size_t count_byte(const uint8_t *h, size_t n, uint8_t c) {
	size_t cnt = 0;
	const uint8_t *p = h;
	const uint8_t *end = h + n;
	while (p < end) {
		const void *q = memchr(p, c, (size_t)(end - p));
		if (!q) break;
		cnt++;
		p = (const uint8_t *)q + 1;
	}
	return cnt;
}

static void horspool_prep(const uint8_t *nd, size_t m, int shift[256]) {
	for (int i = 0; i < 256; i++) shift[i] = (int)m;
	for (size_t i = 0; i + 1 < m; i++) shift[nd[i]] = (int)(m - 1 - i);
}

static int needle_uniform(const uint8_t *nd, size_t m) {
	for (size_t i = 1; i < m; i++)
		if (nd[i] != nd[0]) return 0;
	return 1;
}

static ssize_t memmem_find(const uint8_t *h, size_t n, const uint8_t *nd,
                           size_t m) {
	const void *p = memmem(h, n, nd, m);
	return p ? (ssize_t)((const uint8_t *)p - h) : (ssize_t)-1;
}

static size_t horspool_count(const uint8_t *h, size_t n, const uint8_t *nd,
                             size_t m, int overlap) {
	size_t cnt = 0;
	size_t pos = 0;
	int shift[256];
	horspool_prep(nd, m, shift);
	while (pos + m <= n) {
		if (h[pos + m - 1] == nd[m - 1] && memcmp(h + pos, nd, m) == 0) {
			cnt++;
			pos += overlap ? 1 : m;
		} else {
			pos += (size_t)shift[h[pos + m - 1]];
		}
	}
	return cnt;
}

static ssize_t find_2(const uint8_t *h, size_t n, const uint8_t *nd) {
	if (n < 2) return -1;
	uint8_t a = nd[0], b = nd[1];
	const uint8_t *p = h;
	const uint8_t *end = h + n - 1;
	while (p < end) {
		const void *q = memchr(p, a, (size_t)(end - p));
		if (!q) return -1;
		p = (const uint8_t *)q;
		if (p[1] == b) return (ssize_t)(p - h);
		p++;
	}
	return -1;
}

static ssize_t find_3(const uint8_t *h, size_t n, const uint8_t *nd) {
	if (n < 3) return -1;
	uint8_t a = nd[0];
	const uint8_t *p = h;
	const uint8_t *lim = h + n - 2;
	while (p < lim) {
		const void *q = memchr(p, a, (size_t)(lim - p));
		if (!q) return -1;
		p = (const uint8_t *)q;
		if (p[1] == nd[1] && p[2] == nd[2]) return (ssize_t)(p - h);
		p++;
	}
	return -1;
}

/* ---------- Two-Way (Crochemore–Perrin) ---------- */

static size_t max_suffix_tw(const uint8_t *x, size_t m, int *period, int rev) {
	size_t ms = (size_t)-1, j = 0, k = 1, p = 1;
	while (j + k < m) {
		uint8_t a = x[j + k];
		uint8_t b = x[ms + k];
		int lt = rev ? (a > b) : (a < b);
		int eq = (a == b);
		if (lt) {
			j += k;
			k = 1;
			p = j - ms;
		} else if (eq) {
			if (k == p) {
				j += p;
				k = 1;
			} else {
				k++;
			}
		} else {
			ms = j;
			j = ms + 1;
			k = 1;
			p = 1;
		}
	}
	*period = (int)p;
	return ms + 1;
}

static ssize_t two_way_search(const uint8_t *h, size_t n, const uint8_t *nd,
                              size_t m) {
	int p1 = 1, p2 = 1;
	size_t i = max_suffix_tw(nd, m, &p1, 0);
	size_t j = max_suffix_tw(nd, m, &p2, 1);
	size_t crit, period;
	if (i > j) {
		crit = i;
		period = (size_t)p1;
	} else {
		crit = j;
		period = (size_t)p2;
	}

	int bad[256];
	for (int c = 0; c < 256; c++) bad[c] = (int)m;
	for (size_t k = 0; k + 1 < m; k++) bad[nd[k]] = (int)(m - 1 - k);

	int exact = (memcmp(nd, nd + period, crit) == 0);
	size_t pos = 0;
	size_t memory = 0;

	while (pos + m <= n) {
		size_t ii = (crit > memory) ? crit : memory;
		while (ii < m && nd[ii] == h[pos + ii]) ii++;
		if (ii < m) {
			size_t sh = (size_t)bad[h[pos + m - 1]];
			size_t s2 = ii - crit + 1;
			if (sh < s2) sh = s2;
			pos += sh;
			memory = 0;
		} else {
			size_t jj = crit;
			while (jj > memory && nd[jj - 1] == h[pos + jj - 1]) jj--;
			if (jj <= memory) return (ssize_t)pos;
			if (exact) {
				pos += period;
				memory = m - period;
			} else {
				pos += (crit > period) ? crit : period;
				memory = 0;
			}
		}
	}
	return -1;
}

#ifdef FSS_HAVE_X86
static int g_avx2 = -1;

static int cpu_has_avx2(void) {
	if (g_avx2 >= 0) return g_avx2;
	unsigned int eax, ebx, ecx, edx;
	if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx) || !(ecx & bit_OSXSAVE)) {
		g_avx2 = 0;
		return 0;
	}
	if (!__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
		g_avx2 = 0;
		return 0;
	}
	g_avx2 = (ebx & bit_AVX2) ? 1 : 0;
	return g_avx2;
}

static ssize_t avx2_find(const uint8_t *h, size_t n, const uint8_t *nd,
                         size_t m) {
	uint8_t first = nd[0];
	uint8_t last = nd[m - 1];
	/*
	 * Uniform needles: dual filter collapses to first-byte. Common fill
	 * bytes ('a') drown in false hits — glibc memmem wins. Rare fill
	 * ('=', '-') still prefer first-byte SIMD below.
	 */
	if (needle_uniform(nd, m)) {
		if ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
		    first == ' ' || (first >= '0' && first <= '9'))
			return memmem_find(h, n, nd, m);
		/* rare uniform: first-byte AVX2 (same as dual when first==last) */
	}

	__m256i vfirst = _mm256_set1_epi8((char)first);
	__m256i vlast = _mm256_set1_epi8((char)last);
	int dual = (first != last);
	size_t limit = n - m + 1;
	size_t i = 0;
	for (; i + 32 <= limit; i += 32) {
		__m256i b0 = _mm256_loadu_si256((const __m256i *)(h + i));
		unsigned mask =
		    (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b0, vfirst));
		if (dual) {
			__m256i b1 =
			    _mm256_loadu_si256((const __m256i *)(h + i + (m - 1)));
			mask &= (unsigned)_mm256_movemask_epi8(
			    _mm256_cmpeq_epi8(b1, vlast));
		}
		while (mask) {
			unsigned bit = (unsigned)__builtin_ctz(mask);
			size_t pos = i + bit;
			if ((!dual || h[pos + m - 1] == last) &&
			    memcmp(h + pos, nd, m) == 0)
				return (ssize_t)pos;
			mask &= mask - 1;
		}
	}
	for (; i < limit; i++) {
		if (h[i] == first && h[i + m - 1] == last && memcmp(h + i, nd, m) == 0)
			return (ssize_t)i;
	}
	return -1;
}

static size_t avx2_count(const uint8_t *h, size_t n, const uint8_t *nd,
                         size_t m, int overlap) {
	uint8_t first = nd[0];
	uint8_t last = nd[m - 1];
	if (needle_uniform(nd, m) &&
	    ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
	     first == ' ' || (first >= '0' && first <= '9'))) {
		/* Common fill: Horspool/AVX2 drown — walk glibc memmem. */
		size_t cnt = 0, pos = 0;
		while (pos + m <= n) {
			const void *p = memmem(h + pos, n - pos, nd, m);
			if (!p) break;
			size_t abs = (size_t)((const uint8_t *)p - h);
			cnt++;
			pos = abs + (overlap ? 1 : m);
		}
		return cnt;
	}

	__m256i vfirst = _mm256_set1_epi8((char)first);
	__m256i vlast = _mm256_set1_epi8((char)last);
	int dual = (first != last);
	size_t limit = n - m + 1;
	size_t cnt = 0;
	size_t i = 0;
	while (i < limit) {
		if (i + 32 <= limit) {
			__m256i b0 = _mm256_loadu_si256((const __m256i *)(h + i));
			unsigned mask =
			    (unsigned)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b0, vfirst));
			if (dual) {
				__m256i b1 = _mm256_loadu_si256(
				    (const __m256i *)(h + i + (m - 1)));
				mask &= (unsigned)_mm256_movemask_epi8(
				    _mm256_cmpeq_epi8(b1, vlast));
			}
			if (overlap) {
				while (mask) {
					unsigned bit = (unsigned)__builtin_ctz(mask);
					size_t p = i + bit;
					if (p < limit &&
					    (!dual || h[p + m - 1] == last) &&
					    memcmp(h + p, nd, m) == 0)
						cnt++;
					mask &= mask - 1;
				}
				i += 32;
				continue;
			}
			int advanced = 0;
			while (mask) {
				unsigned bit = (unsigned)__builtin_ctz(mask);
				size_t p = i + bit;
				if (p >= limit) break;
				if ((!dual || h[p + m - 1] == last) &&
				    memcmp(h + p, nd, m) == 0) {
					cnt++;
					i = p + m;
					advanced = 1;
					break;
				}
				mask &= mask - 1;
			}
			if (!advanced) i += 32;
			continue;
		}
		if (h[i] == first && h[i + m - 1] == last &&
		    memcmp(h + i, nd, m) == 0) {
			cnt++;
			i += overlap ? 1 : m;
		} else {
			i++;
		}
	}
	return cnt;
}
#endif

ssize_t fss_find_raw(const uint8_t *h, size_t n, const uint8_t *nd, size_t m) {
	if (m == 0) return 0;
	if (m > n) return -1;
	if (n < 64) return memmem_find(h, n, nd, m);
	if (m == 1) return find_byte(h, n, nd[0]);
	/*
	 * m=2,3: memchr dual/triple scan. AVX2 first-byte filter loses when nd[0]
	 * is common ('-', '?', ']') — many false hits (~2–30× slower than memmem
	 * on enwik closer misses). find_2/3 beat memmem on those cases.
	 */
	if (m == 2) return find_2(h, n, nd);
	if (m == 3) return find_3(h, n, nd);
#ifdef FSS_HAVE_X86
	if (cpu_has_avx2()) return avx2_find(h, n, nd, m);
#endif
	return two_way_search(h, n, nd, m);
}

size_t fss_count_raw(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                     int overlap) {
	if (m == 0) return 0;
	if (m > n) return 0;
	if (m == 1) return count_byte(h, n, nd[0]);

#ifdef FSS_HAVE_X86
	/* m≥2: dual first+last AVX2. Beats memchr walk on dense short needles
	 * (enwik "the"/"th" ~3–4×); rare closers stay ~memchr. */
	if (n >= 64 && m >= 2 && cpu_has_avx2()) return avx2_count(h, n, nd, m, overlap);
#endif

	/* Short needles without AVX2: memchr walk. */
	if (m <= 3) {
		size_t cnt = 0;
		size_t pos = 0;
		uint8_t a = nd[0];
		while (pos + m <= n) {
			const void *q = memchr(h + pos, a, n - pos);
			if (!q) break;
			size_t p = (size_t)((const uint8_t *)q - h);
			if (p + m > n) break;
			if ((m == 2 && h[p + 1] == nd[1]) ||
			    (m == 3 && h[p + 1] == nd[1] && h[p + 2] == nd[2])) {
				cnt++;
				pos = p + (overlap ? 1 : m);
			} else {
				pos = p + 1;
			}
		}
		return cnt;
	}

	return horspool_count(h, n, nd, m, overlap);
}

int fss_should_use(size_t hay_n, size_t needle_m, int kind) {
	(void)needle_m;
	if (kind == FSS_KIND_REPEATS) return hay_n >= 64;
	/* Prefer libc for tiny finds from callers that ask. */
	if (kind == FSS_KIND_FIND && hay_n < 64) return 0;
	return 1;
}

ssize_t fss_find(const uint8_t *h, size_t n, const uint8_t *nd, size_t m) {
	return fss_find_ex(h, n, nd, m, NULL);
}

ssize_t fss_find_ex(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                    const fss_profile *prof) {
	if (m == 0) return 0;
	if (m > n) return -1;
	if (prof && !fss_profile_may_contain(prof, nd, m)) return -1;

	/* Immense: saccade only into L2-surviving blocks when selective. */
	if (prof && prof->blooms && prof->nblocks > 4 && n >= 65536 && m >= 4 &&
	    m + 1 < prof->block_bytes) {
		size_t idxs[256];
		size_t nb = fss_profile_candidate_blocks(prof, nd, m, idxs, 256);
		if (nb != (size_t)-1 && nb > 0 && nb * 2 < prof->nblocks) {
			ssize_t best = -1;
			for (size_t i = 0; i < nb; i++) {
				size_t b = idxs[i];
				size_t off = b * prof->block_bytes;
				size_t len = prof->block_bytes;
				if (off >= n) break;
				if (off + len > n) len = n - off;
				/* Overlap previous block by m-1 so boundary matches hit. */
				size_t start = off > (m - 1) ? off - (m - 1) : 0;
				size_t end = off + len;
				if (end > n) end = n;
				ssize_t f = fss_find_raw(h + start, end - start, nd, m);
				if (f >= 0) {
					ssize_t abs = (ssize_t)start + f;
					if (best < 0 || abs < best) best = abs;
					/* First occurrence overall: can early-out if scanning
					 * in order — idxs are ascending. */
					return best;
				}
			}
			return -1;
		}
	}
	return fss_find_raw(h, n, nd, m);
}

size_t fss_count(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                 int overlap) {
	return fss_count_ex(h, n, nd, m, overlap, NULL);
}

size_t fss_count_ex(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                    int overlap, const fss_profile *prof) {
	if (m == 0) return 0;
	if (m > n) return 0;
	if (prof && !fss_profile_may_contain(prof, nd, m)) return 0;
	return fss_count_raw(h, n, nd, m, overlap);
}

size_t fss_find_all(const uint8_t *h, size_t n, const uint8_t *nd, size_t m,
                    int overlap, size_t *offsets, size_t out_cap) {
	if (m == 0 || m > n || !offsets || out_cap == 0) return 0;

	/* m==1: memchr collect */
	if (m == 1) {
		size_t written = 0;
		const uint8_t *p = h;
		const uint8_t *end = h + n;
		uint8_t c = nd[0];
		while (p < end && written < out_cap) {
			const void *q = memchr(p, c, (size_t)(end - p));
			if (!q) break;
			offsets[written++] = (size_t)((const uint8_t *)q - h);
			p = (const uint8_t *)q + 1;
		}
		return written;
	}

#ifdef FSS_HAVE_X86
	if (n >= 64 && cpu_has_avx2()) {
		uint8_t first = nd[0];
		uint8_t last = nd[m - 1];
		if (needle_uniform(nd, m) &&
		    ((first >= 'a' && first <= 'z') || (first >= 'A' && first <= 'Z') ||
		     first == ' ' || (first >= '0' && first <= '9'))) {
			size_t written = 0, pos = 0;
			while (pos + m <= n && written < out_cap) {
				const void *p = memmem(h + pos, n - pos, nd, m);
				if (!p) break;
				size_t abs = (size_t)((const uint8_t *)p - h);
				offsets[written++] = abs;
				pos = abs + (overlap ? 1 : m);
			}
			return written;
		}
		__m256i vfirst = _mm256_set1_epi8((char)first);
		__m256i vlast = _mm256_set1_epi8((char)last);
		int dual = (first != last);
		size_t limit = n - m + 1;
		size_t written = 0;
		size_t i = 0;
		while (i < limit && written < out_cap) {
			if (i + 32 <= limit) {
				__m256i b0 = _mm256_loadu_si256((const __m256i *)(h + i));
				unsigned mask = (unsigned)_mm256_movemask_epi8(
				    _mm256_cmpeq_epi8(b0, vfirst));
				if (dual) {
					__m256i b1 = _mm256_loadu_si256(
					    (const __m256i *)(h + i + (m - 1)));
					mask &= (unsigned)_mm256_movemask_epi8(
					    _mm256_cmpeq_epi8(b1, vlast));
				}
				if (overlap) {
					while (mask && written < out_cap) {
						unsigned bit = (unsigned)__builtin_ctz(mask);
						size_t p = i + bit;
						if (p < limit &&
						    (!dual || h[p + m - 1] == last) &&
						    memcmp(h + p, nd, m) == 0)
							offsets[written++] = p;
						mask &= mask - 1;
					}
					i += 32;
					continue;
				}
				int advanced = 0;
				while (mask) {
					unsigned bit = (unsigned)__builtin_ctz(mask);
					size_t p = i + bit;
					if (p >= limit) break;
					if ((!dual || h[p + m - 1] == last) &&
					    memcmp(h + p, nd, m) == 0) {
						offsets[written++] = p;
						i = p + m;
						advanced = 1;
						break;
					}
					mask &= mask - 1;
				}
				if (!advanced) i += 32;
				continue;
			}
			if (h[i] == first && h[i + m - 1] == last &&
			    memcmp(h + i, nd, m) == 0) {
				offsets[written++] = i;
				i += overlap ? 1 : m;
			} else {
				i++;
			}
		}
		return written;
	}
#endif

	/* Horspool collect */
	{
		size_t written = 0, pos = 0;
		int shift[256];
		horspool_prep(nd, m, shift);
		while (pos + m <= n && written < out_cap) {
			if (h[pos + m - 1] == nd[m - 1] && memcmp(h + pos, nd, m) == 0) {
				offsets[written++] = pos;
				pos += overlap ? 1 : m;
			} else {
				pos += (size_t)shift[h[pos + m - 1]];
			}
		}
		return written;
	}
}
