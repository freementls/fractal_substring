/*
 * bench/bench_fss.c — small Pareto grid vs memmem.
 */
#include "fss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static double now_s(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void fill_random(uint8_t *p, size_t n, unsigned seed) {
	for (size_t i = 0; i < n; i++) {
		seed = seed * 1103515245u + 12345u;
		p[i] = (uint8_t)(seed >> 16);
	}
}

static void fill_englishish(uint8_t *p, size_t n) {
	static const char *words[] = {"the ", "quick ", "brown ", "fox ", "jumps ",
	                              "over ", "lazy ", "dog ", "and ", "then "};
	size_t pos = 0;
	size_t wi = 0;
	while (pos < n) {
		const char *w = words[wi++ % 10];
		size_t L = strlen(w);
		if (pos + L > n) L = n - pos;
		memcpy(p + pos, w, L);
		pos += L;
	}
}

static void bench_one(const char *tag, const uint8_t *h, size_t n,
                      const uint8_t *nd, size_t m) {
	const int rounds = n < 4096 ? 20000 : (n < (1u << 20) ? 200 : 20);
	volatile ssize_t sink = 0;

	double t0 = now_s();
	for (int i = 0; i < rounds; i++) sink += fss_find(h, n, nd, m);
	double t1 = now_s();

	double t2 = now_s();
	for (int i = 0; i < rounds; i++) {
		const void *p = memmem(h, n, nd, m);
		sink += p ? 1 : 0;
	}
	double t3 = now_s();

	double fss_ns = (t1 - t0) * 1e9 / rounds;
	double mm_ns = (t3 - t2) * 1e9 / rounds;
	printf("%-12s n=%8zu m=%3zu  fss=%8.1f ns  memmem=%8.1f ns  ratio=%.2fx  "
	       "(sink=%zd)\n",
	       tag, n, m, fss_ns, mm_ns, mm_ns > 0 ? fss_ns / mm_ns : 0.0, sink);
}

int main(void) {
	size_t sizes[] = {16, 1024, 64 * 1024, 1024 * 1024};
	size_t needles[] = {1, 2, 3, 8, 32};

	for (size_t si = 0; si < sizeof sizes / sizeof sizes[0]; si++) {
		size_t n = sizes[si];
		uint8_t *h = malloc(n);
		fill_englishish(h, n);
		for (size_t ni = 0; ni < sizeof needles / sizeof needles[0]; ni++) {
			size_t m = needles[ni];
			if (m > n) continue;
			uint8_t *nd = malloc(m);
			memcpy(nd, h + n / 3, m); /* guaranteed hit */
			bench_one("english", h, n, nd, m);
			free(nd);
		}
		/* miss */
		uint8_t miss[8] = {0xff, 0xfe, 0xfd, 0xfc, 0xfb, 0xfa, 0xf9, 0xf8};
		bench_one("eng-miss", h, n, miss, 8 < n ? 8 : 1);
		free(h);
	}

	/* count bench */
	{
		size_t n = 256 * 1024;
		uint8_t *h = malloc(n);
		fill_englishish(h, n);
		const uint8_t *nd = (const uint8_t *)"the ";
		double t0 = now_s();
		size_t c = 0;
		for (int i = 0; i < 50; i++) c += fss_count(h, n, nd, 4, 0);
		double t1 = now_s();
		printf("count 256KiB 'the ' x50: %.3f ms total, last~%zu\n",
		       (t1 - t0) * 1e3, c / 50);
		free(h);
	}
	return 0;
}
