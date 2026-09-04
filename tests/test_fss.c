/*
 * tests/test_fss.c — correctness vs memmem / substr_count semantics.
 */
#include "fss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

static void expect_ss(const char *name, ssize_t got, ssize_t want) {
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %zd want %zd\n", name, got, want);
		failures++;
	}
}

static void expect_zu(const char *name, size_t got, size_t want) {
	if (got != want) {
		fprintf(stderr, "FAIL %s: got %zu want %zu\n", name, got, want);
		failures++;
	}
}

static void expect_true(const char *name, int cond) {
	if (!cond) {
		fprintf(stderr, "FAIL %s\n", name);
		failures++;
	}
}

static void test_find_basic(void) {
	const uint8_t *h = (const uint8_t *)"hello world hello";
	size_t n = strlen((const char *)h);
	expect_ss("empty needle", fss_find(h, n, (const uint8_t *)"", 0), 0);
	expect_ss("miss", fss_find(h, n, (const uint8_t *)"xyz", 3), -1);
	expect_ss("hello", fss_find(h, n, (const uint8_t *)"hello", 5), 0);
	expect_ss("world", fss_find(h, n, (const uint8_t *)"world", 5), 6);
	expect_ss("byte", fss_find(h, n, (const uint8_t *)"w", 1), 6);
	expect_ss("two", fss_find(h, n, (const uint8_t *)"ll", 2), 2);
	expect_ss("three", fss_find(h, n, (const uint8_t *)"llo", 3), 2);
}

static void test_vs_memmem(void) {
	const char *texts[] = {
	    "",
	    "a",
	    "abcdefghijklmnopqrstuvwxyz",
	    "The quick brown fox jumps over the lazy dog",
	    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
	    "abcabcabcabcabcabcabcabc",
	};
	const char *needles[] = {"", "a", "abc", "xyz", "fox", "aaa", "lazy", "zz"};
	for (size_t ti = 0; ti < sizeof texts / sizeof texts[0]; ti++) {
		const uint8_t *h = (const uint8_t *)texts[ti];
		size_t n = strlen(texts[ti]);
		for (size_t ni = 0; ni < sizeof needles / sizeof needles[0]; ni++) {
			const uint8_t *nd = (const uint8_t *)needles[ni];
			size_t m = strlen(needles[ni]);
			ssize_t got = fss_find(h, n, nd, m);
			ssize_t want;
			if (m == 0)
				want = 0;
			else {
				const void *p = memmem(h, n, nd, m);
				want = p ? (ssize_t)((const uint8_t *)p - h) : (ssize_t)-1;
			}
			char name[128];
			snprintf(name, sizeof name, "memmem t%zu n%zu", ti, ni);
			expect_ss(name, got, want);
		}
	}
}

static void test_count_php(void) {
	/* PHP substr_count is non-overlapping */
	const uint8_t *h = (const uint8_t *)"aaaa";
	expect_zu("aaaa/aa nonoverlap", fss_count(h, 4, (const uint8_t *)"aa", 2, 0),
	          2);
	expect_zu("aaaa/aa overlap", fss_count(h, 4, (const uint8_t *)"aa", 2, 1), 3);
	const uint8_t *h2 = (const uint8_t *)"abcabcabc";
	expect_zu("abc x3", fss_count(h2, 9, (const uint8_t *)"abc", 3, 0), 3);
}

static void test_profile_reject(void) {
	const uint8_t *h = (const uint8_t *)"hello world";
	fss_profile *p = fss_profile_build(h, 11, 2);
	expect_true("profile", p != NULL);
	expect_true("may hello", fss_profile_may_contain(p, (const uint8_t *)"hello", 5));
	expect_true("reject z", !fss_profile_may_contain(p, (const uint8_t *)"xyz", 3));
	expect_ss("find reject", fss_find_ex(h, 11, (const uint8_t *)"xyz", 3, p), -1);
	fss_profile_free(p);
}

static void test_batch(void) {
	const uint8_t *h = (const uint8_t *)"the quick brown fox";
	size_t n = strlen((const char *)h);
	const uint8_t *nds[] = {
	    (const uint8_t *)"quick",
	    (const uint8_t *)"slow",
	    (const uint8_t *)"fox",
	    (const uint8_t *)"",
	};
	size_t ms[] = {5, 4, 3, 0};
	uint8_t out[4];
	fss_has_batch(h, n, nds, ms, 4, out);
	expect_zu("batch quick", out[0], 1);
	expect_zu("batch slow", out[1], 0);
	expect_zu("batch fox", out[2], 1);
	expect_zu("batch empty", out[3], 1);
}

static void test_repeats(void) {
	const uint8_t *h =
	    (const uint8_t *)"abcXYZabcXYZabc---hellohello---abcXYZ";
	size_t n = strlen((const char *)h);
	fss_repeat_opts o = {.min_len = 3, .max_len = 16, .min_count = 2, .top_k = 8};
	fss_repeat out[8];
	size_t nr = fss_repeats(h, n, &o, out, 8);
	expect_true("repeats found", nr > 0);
	int saw = 0;
	for (size_t i = 0; i < nr; i++) {
		if (out[i].count >= 2) saw = 1;
	}
	expect_true("repeat count>=2", saw);
}

/* Immense: CDC twins across distant blocks (no full-n SA). */
static void test_repeats_immense(void) {
	size_t tile = 4096;
	size_t n = tile * 64; /* 256 KiB */
	uint8_t *h = malloc(n);
	expect_true("immense alloc", h != NULL);
	for (size_t i = 0; i < tile; i++) h[i] = (uint8_t)(i * 17 + 3);
	/* Plant identical tiles at block 0 and block 40. */
	memcpy(h + 40 * tile, h, tile);
	/* Fill rest with low-redundancy noise. */
	for (size_t i = tile; i < 40 * tile; i++) h[i] = (uint8_t)(i * 31);
	for (size_t i = 41 * tile; i < n; i++) h[i] = (uint8_t)(i * 29 + 7);

	fss_repeat_opts o = {
	    .min_len = 64, .max_len = 8192, .min_count = 2, .top_k = 8};
	fss_repeat out[8];
	size_t nr = fss_repeats(h, n, &o, out, 8);
	expect_true("immense repeats", nr > 0);
	int saw_long = 0;
	for (size_t i = 0; i < nr; i++) {
		if (out[i].len >= 1024 && out[i].count >= 2) saw_long = 1;
	}
	expect_true("immense long twin", saw_long);
	free(h);
}

static void test_l2_find(void) {
	size_t n = 128 * 1024;
	uint8_t *h = malloc(n);
	expect_true("l2 alloc", h != NULL);
	memset(h, 'x', n);
	const char *nd = "UNIQUE_NEEDLE_ZZ";
	size_t m = strlen(nd);
	memcpy(h + 100000, nd, m);
	fss_profile *p = fss_profile_build(h, n, 2);
	ssize_t got = fss_find_ex(h, n, (const uint8_t *)nd, m, p);
	expect_ss("l2 find", got, 100000);
	fss_profile_free(p);
	free(h);
}

static void test_store(void) {
	fss_store *s = fss_store_create();
	expect_true("store", s != NULL);
	const char *text = "memoized haystack with needle needle";
	fss_handle h = fss_intern(s, text, strlen(text));
	ssize_t a = fss_find_in(s, h, (const uint8_t *)"needle", 6);
	ssize_t b = fss_find_in(s, h, (const uint8_t *)"needle", 6); /* cache hit */
	expect_true("find_in", a >= 0);
	expect_ss("cache same", a, b);
	size_t c = fss_count_in(s, h, (const uint8_t *)"needle", 6, 0);
	expect_zu("count_in", c, 2);
	fss_release(s, h);
	fss_store_free(s);
}

static void test_sa_and_repeats_memo(void) {
	/* High-redundancy medium string — L4 SA path. */
	char *tile = "THE_QUICK_BROWN_FOX_";
	size_t tn = strlen(tile);
	size_t n = tn * 200; /* ~4 KiB */
	uint8_t *h = malloc(n);
	expect_true("sa alloc", h != NULL);
	for (size_t i = 0; i < n; i++) h[i] = (uint8_t)tile[i % tn];

	fss_repeat_opts o = {
	    .min_len = 8, .max_len = 64, .min_count = 2, .top_k = 8};
	fss_repeat out[8];
	size_t nr = fss_repeats(h, n, &o, out, 8);
	expect_true("sa repeats", nr > 0);

	fss_store *s = fss_store_create();
	fss_handle hh = fss_intern(s, h, n);
	size_t a = fss_repeats_in(s, hh, &o, out, 8);
	expect_true("repeats_in cold", a > 0);
	size_t b = fss_repeats_in(s, hh, &o, out, 8);
	expect_zu("repeats_in warm count", b, a);
	expect_true("repeats_in ptr", out[0].p != NULL && out[0].count >= 2);

	const uint8_t *nds[] = {(const uint8_t *)"THE_", (const uint8_t *)"NOPE"};
	size_t ms[] = {4, 4};
	uint8_t bits[2];
	fss_has_batch_in(s, hh, nds, ms, 2, bits);
	expect_zu("has_in THE", bits[0], 1);
	expect_zu("has_in NOPE", bits[1], 0);
	fss_has_batch_in(s, hh, nds, ms, 2, bits); /* warm */
	expect_zu("has_in warm", bits[0], 1);

	fss_release(s, hh);
	fss_store_free(s);
	free(h);
}

static void test_count_batch(void) {
	const uint8_t *h = (const uint8_t *)"abcabcabcXYZ";
	const uint8_t *nds[] = {
	    (const uint8_t *)"abc",
	    (const uint8_t *)"XYZ",
	    (const uint8_t *)"nope",
	};
	size_t ms[] = {3, 3, 4};
	uint32_t out[3];
	fss_count_batch(h, 12, nds, ms, 3, 0, out);
	expect_zu("cb abc", out[0], 3);
	expect_zu("cb XYZ", out[1], 1);
	expect_zu("cb nope", out[2], 0);
}

/* Multi-length count in one pass — must match per-needle fss_count. */
static void test_count_batch_multilen(void) {
	const uint8_t *h = (const uint8_t *)"abXabYYabZZZab";
	size_t n = strlen((const char *)h);
	const uint8_t *nds[] = {
	    (const uint8_t *)"ab",
	    (const uint8_t *)"YY",
	    (const uint8_t *)"ZZZ",
	    (const uint8_t *)"abZ",
	    (const uint8_t *)"qq",
	};
	size_t ms[] = {2, 2, 3, 3, 2};
	uint32_t out[5];
	fss_count_batch(h, n, nds, ms, 5, 0, out);
	for (size_t i = 0; i < 5; i++) {
		size_t want = fss_count(h, n, nds[i], ms[i], 0);
		char name[64];
		snprintf(name, sizeof name, "cml %zu", i);
		expect_zu(name, out[i], want);
	}
}

/* Large batch triggers Aho-Corasick path (k≥32, n≥4096). */
static void test_has_batch_ac(void) {
	size_t n = 8192;
	uint8_t *h = malloc(n);
	expect_true("ac hay alloc", h != NULL);
	for (size_t i = 0; i < n; i++) h[i] = (uint8_t)('a' + (i % 26));

	enum { K = 64 };
	uint8_t blob[K * 8];
	const uint8_t *nds[K];
	size_t ms[K];
	uint8_t bits[K];
	uint8_t want[K];

	for (int i = 0; i < K; i++) {
		size_t m = 3 + (size_t)(i % 5);
		ms[i] = m;
		nds[i] = blob + (size_t)i * 8;
		if (i < 40) {
			/* Present: copy from haystack */
			size_t off = (size_t)(i * 97) % (n - m);
			memcpy((void *)nds[i], h + off, m);
			want[i] = 1;
		} else {
			/* Absent: zz + unique digits (trigram-rare) */
			snprintf((char *)(void *)nds[i], 8, "zz%04d", i);
			want[i] = 0;
		}
	}
	/* Confirm want[] against memmem for presents/absents */
	for (int i = 0; i < K; i++) {
		const void *p = memmem(h, n, nds[i], ms[i]);
		want[i] = p ? 1 : 0;
	}

	fss_has_batch(h, n, nds, ms, K, bits);
	for (int i = 0; i < K; i++) {
		char name[64];
		snprintf(name, sizeof name, "ac has %d", i);
		expect_zu(name, bits[i], want[i]);
	}
	free(h);
}

static void test_two_way_adversarial(void) {
	/* Periodic pattern that stresses naive/Horspool */
	size_t n = 10000;
	uint8_t *h = malloc(n);
	memset(h, 'a', n);
	h[n - 1] = 'b';
	const uint8_t *nd = (const uint8_t *)"aaaab";
	ssize_t got = fss_find(h, n, nd, 5);
	const void *p = memmem(h, n, nd, 5);
	ssize_t want = p ? (ssize_t)((const uint8_t *)p - h) : (ssize_t)-1;
	expect_ss("tw adversarial", got, want);
	free(h);
}

static void test_find_all(void) {
	const uint8_t *h = (const uint8_t *)"ababa bababa";
	size_t n = 12;
	size_t offs[16];
	size_t c = fss_find_all(h, n, (const uint8_t *)"aba", 3, 0, offs, 16);
	expect_zu("find_all non-ov", c, 2);
	expect_zu("find_all[0]", offs[0], 0);
	expect_zu("find_all[1]", offs[1], 7);
	c = fss_find_all(h, n, (const uint8_t *)"aba", 3, 1, offs, 16);
	size_t cnt = fss_count(h, n, (const uint8_t *)"aba", 3, 1);
	expect_zu("find_all ov == count", c, cnt);
	expect_true("find_all ov >= 3", c >= 3);
}

int main(void) {
	test_find_basic();
	test_vs_memmem();
	test_count_php();
	test_profile_reject();
	test_batch();
	test_repeats();
	test_repeats_immense();
	test_l2_find();
	test_store();
	test_sa_and_repeats_memo();
	test_count_batch();
	test_count_batch_multilen();
	test_has_batch_ac();
	test_two_way_adversarial();
	test_find_all();
	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("ok\n");
	return 0;
}
