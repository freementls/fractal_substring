/*
 * fss_batch.c — batch substring-presence / count (hastok successor).
 *
 * has_batch: Aho-Corasick when k≥96 & n≥4KiB; else sequential fss_find
 * (early-exit beats AC and the old multi-length roller on smaller batches).
 * count_batch: Aho-Corasick when k≥48 & n≥4KiB; else per-needle fss_count.
 */
#include "fss_internal.h"

#include <stdlib.h>
#include <string.h>

#define AC_NO (-1)

struct ac_node {
	int next[256];
	int fail;
	int out; /* head into out_idx/out_next, or AC_NO */
};

static void ac_node_init(struct ac_node *n) {
	for (int c = 0; c < 256; c++) n->next[c] = AC_NO;
	n->fail = 0;
	n->out = AC_NO;
}

/* Aho-Corasick presence: build once, scan haystack once. */
static int has_scan_ac(const uint8_t *h, size_t n, const uint8_t *blob,
                       const uint32_t *offs, const uint32_t *lens, size_t k,
                       uint8_t *out_bits) {
	size_t tot = 0;
	size_t remaining = 0;
	for (size_t i = 0; i < k; i++) {
		tot += lens[i];
		if (lens[i] == 0)
			out_bits[i] = 1;
		else if ((size_t)lens[i] <= n)
			remaining++;
	}
	if (!remaining) return 1;

	size_t cap = tot + 2;
	if (cap < 64) cap = 64;
	/* Dense next[] ≈ 1 KiB/node — refuse absurd automata. */
	if (cap > 256u * 1024u) return 0;

	struct ac_node *nodes =
	    (struct ac_node *)malloc(cap * sizeof(struct ac_node));
	if (!nodes) return 0;
	ac_node_init(&nodes[0]);
	int nnodes = 1;

	int *out_idx = (int *)malloc((k + 1) * sizeof(int));
	int *out_next = (int *)malloc((k + 1) * sizeof(int));
	if (!out_idx || !out_next) {
		free(nodes);
		free(out_idx);
		free(out_next);
		return 0;
	}
	int nout = 0;

	for (size_t i = 0; i < k; i++) {
		if (lens[i] == 0 || (size_t)lens[i] > n) continue;
		int u = 0;
		for (uint32_t j = 0; j < lens[i]; j++) {
			unsigned c = blob[offs[i] + j];
			if (nodes[u].next[c] == AC_NO) {
				if ((size_t)nnodes >= cap) {
					size_t ncap = cap * 2;
					if (ncap > 256u * 1024u) {
						free(nodes);
						free(out_idx);
						free(out_next);
						return 0;
					}
					struct ac_node *nn = (struct ac_node *)realloc(
					    nodes, ncap * sizeof(struct ac_node));
					if (!nn) {
						free(nodes);
						free(out_idx);
						free(out_next);
						return 0;
					}
					nodes = nn;
					cap = ncap;
				}
				ac_node_init(&nodes[nnodes]);
				nodes[u].next[c] = nnodes++;
			}
			u = nodes[u].next[c];
		}
		out_idx[nout] = (int)i;
		out_next[nout] = nodes[u].out;
		nodes[u].out = nout;
		nout++;
	}

	int *q = (int *)malloc((size_t)nnodes * sizeof(int));
	if (!q) {
		free(nodes);
		free(out_idx);
		free(out_next);
		return 0;
	}
	int qh = 0, qt = 0;
	for (int c = 0; c < 256; c++) {
		if (nodes[0].next[c] != AC_NO) {
			nodes[nodes[0].next[c]].fail = 0;
			q[qt++] = nodes[0].next[c];
		} else {
			nodes[0].next[c] = 0; /* complete trie */
		}
	}
	while (qh < qt) {
		int u = q[qh++];
		for (int c = 0; c < 256; c++) {
			int v = nodes[u].next[c];
			if (v == AC_NO) {
				nodes[u].next[c] = nodes[nodes[u].fail].next[c];
				continue;
			}
			nodes[v].fail = nodes[nodes[u].fail].next[c];
			if (nodes[v].out == AC_NO) {
				nodes[v].out = nodes[nodes[v].fail].out;
			} else {
				int t = nodes[v].out;
				while (out_next[t] != AC_NO) t = out_next[t];
				out_next[t] = nodes[nodes[v].fail].out;
			}
			q[qt++] = v;
		}
	}
	free(q);

	int s = 0;
	for (size_t i = 0; i < n && remaining; i++) {
		s = nodes[s].next[h[i]];
		for (int o = nodes[s].out; o != AC_NO; o = out_next[o]) {
			int pi = out_idx[o];
			if (!out_bits[pi]) {
				out_bits[pi] = 1;
				if (--remaining == 0) break;
			}
		}
	}

	free(nodes);
	free(out_idx);
	free(out_next);
	return 1;
}

/* Aho-Corasick count: same automaton, per-pattern next_ok for overlap policy. */
static int count_scan_ac(const uint8_t *h, size_t n, const uint8_t *blob,
                         const uint32_t *offs, const uint32_t *lens, size_t k,
                         int overlap, uint32_t *out_counts) {
	size_t tot = 0;
	size_t live = 0;
	for (size_t i = 0; i < k; i++) {
		tot += lens[i];
		if (lens[i] > 0 && (size_t)lens[i] <= n) live++;
	}
	if (!live) return 1;

	size_t cap = tot + 2;
	if (cap < 64) cap = 64;
	if (cap > 256u * 1024u) return 0;

	struct ac_node *nodes =
	    (struct ac_node *)malloc(cap * sizeof(struct ac_node));
	if (!nodes) return 0;
	ac_node_init(&nodes[0]);
	int nnodes = 1;

	int *out_idx = (int *)malloc((k + 1) * sizeof(int));
	int *out_next = (int *)malloc((k + 1) * sizeof(int));
	size_t *next_ok = (size_t *)calloc(k, sizeof(size_t));
	if (!out_idx || !out_next || !next_ok) {
		free(nodes);
		free(out_idx);
		free(out_next);
		free(next_ok);
		return 0;
	}
	int nout = 0;

	for (size_t i = 0; i < k; i++) {
		if (lens[i] == 0 || (size_t)lens[i] > n) continue;
		int u = 0;
		for (uint32_t j = 0; j < lens[i]; j++) {
			unsigned c = blob[offs[i] + j];
			if (nodes[u].next[c] == AC_NO) {
				if ((size_t)nnodes >= cap) {
					size_t ncap = cap * 2;
					if (ncap > 256u * 1024u) {
						free(nodes);
						free(out_idx);
						free(out_next);
						free(next_ok);
						return 0;
					}
					struct ac_node *nn = (struct ac_node *)realloc(
					    nodes, ncap * sizeof(struct ac_node));
					if (!nn) {
						free(nodes);
						free(out_idx);
						free(out_next);
						free(next_ok);
						return 0;
					}
					nodes = nn;
					cap = ncap;
				}
				ac_node_init(&nodes[nnodes]);
				nodes[u].next[c] = nnodes++;
			}
			u = nodes[u].next[c];
		}
		out_idx[nout] = (int)i;
		out_next[nout] = nodes[u].out;
		nodes[u].out = nout;
		nout++;
	}

	int *q = (int *)malloc((size_t)nnodes * sizeof(int));
	if (!q) {
		free(nodes);
		free(out_idx);
		free(out_next);
		free(next_ok);
		return 0;
	}
	int qh = 0, qt = 0;
	for (int c = 0; c < 256; c++) {
		if (nodes[0].next[c] != AC_NO) {
			nodes[nodes[0].next[c]].fail = 0;
			q[qt++] = nodes[0].next[c];
		} else {
			nodes[0].next[c] = 0;
		}
	}
	while (qh < qt) {
		int u = q[qh++];
		for (int c = 0; c < 256; c++) {
			int v = nodes[u].next[c];
			if (v == AC_NO) {
				nodes[u].next[c] = nodes[nodes[u].fail].next[c];
				continue;
			}
			nodes[v].fail = nodes[nodes[u].fail].next[c];
			if (nodes[v].out == AC_NO) {
				nodes[v].out = nodes[nodes[v].fail].out;
			} else {
				int t = nodes[v].out;
				while (out_next[t] != AC_NO) t = out_next[t];
				out_next[t] = nodes[nodes[v].fail].out;
			}
			q[qt++] = v;
		}
	}
	free(q);

	int s = 0;
	for (size_t i = 0; i < n; i++) {
		s = nodes[s].next[h[i]];
		for (int o = nodes[s].out; o != AC_NO; o = out_next[o]) {
			int pi = out_idx[o];
			uint32_t len = lens[pi];
			size_t start = i + 1 - (size_t)len;
			if (start >= next_ok[pi]) {
				out_counts[pi]++;
				next_ok[pi] = start + (overlap ? 1u : (size_t)len);
			}
		}
	}

	free(nodes);
	free(out_idx);
	free(out_next);
	free(next_ok);
	return 1;
}

void fss_has_batch_blob(const uint8_t *h, size_t n, const uint8_t *blob,
                        const uint32_t *offs, const uint32_t *lens, size_t k,
                        uint8_t *out_bits) {
	if (!out_bits) return;
	memset(out_bits, 0, k ? k : 1);
	if (k == 0) return;

	/*
	 * Dispatch (enwik4 measured): AC wins once k ≳ 100 (one scan vs many
	 * miss walks). Below that, sequential fss_find early-exits far beat AC
	 * and the old multi-length roller.
	 */
	if (k >= 96 && n >= 4096) {
		if (has_scan_ac(h, n, blob, offs, lens, k, out_bits)) return;
		memset(out_bits, 0, k);
	}

	uint8_t presence[32];
	/* Presence is O(n): only pay it on modest haystacks. Large-n sequential
	 * find/count already early-exit / SIMD-filter; a full presence pass loses. */
	int use_presence = (n > 0 && n < 256u * 1024u);
	if (use_presence) {
		memset(presence, 0, sizeof presence);
		for (size_t i = 0; i < n; i++) fss_presence_set(presence, h[i]);
	} else {
		memset(presence, 0xff, sizeof presence);
	}

	for (size_t i = 0; i < k; i++) {
		uint32_t tl = lens[i];
		if (tl == 0) {
			out_bits[i] = 1;
			continue;
		}
		if ((size_t)tl > n) continue;
		int ok = 1;
		if (use_presence) {
			for (uint32_t j = 0; j < tl; j++) {
				if (!fss_presence_has(presence, blob[offs[i] + j])) {
					ok = 0;
					break;
				}
			}
		}
		if (!ok) continue;
		out_bits[i] = fss_find_raw(h, n, blob + offs[i], tl) >= 0;
	}
}

void fss_has_batch(const uint8_t *h, size_t n, const uint8_t *const *nds,
                   const size_t *ms, size_t k, uint8_t *out_bits) {
	if (!out_bits) return;
	if (k == 0) return;

	size_t total = 0;
	for (size_t i = 0; i < k; i++) total += ms[i];
	uint8_t *blob = (uint8_t *)malloc(total ? total : 1);
	uint32_t *offs = (uint32_t *)malloc(k * sizeof(uint32_t));
	uint32_t *lens = (uint32_t *)malloc(k * sizeof(uint32_t));
	if (!blob || !offs || !lens) {
		free(blob);
		free(offs);
		free(lens);
		memset(out_bits, 0, k);
		return;
	}
	size_t cur = 0;
	for (size_t i = 0; i < k; i++) {
		offs[i] = (uint32_t)cur;
		lens[i] = (uint32_t)ms[i];
		if (ms[i]) memcpy(blob + cur, nds[i], ms[i]);
		cur += ms[i];
	}
	fss_has_batch_blob(h, n, blob, offs, lens, k, out_bits);
	free(blob);
	free(offs);
	free(lens);
}

void fss_count_batch_blob(const uint8_t *h, size_t n, const uint8_t *blob,
                          const uint32_t *offs, const uint32_t *lens, size_t k,
                          int overlap, uint32_t *out_counts) {
	if (!out_counts) return;
	memset(out_counts, 0, k * sizeof(uint32_t));
	if (k == 0 || !h) return;

	/*
	 * Count has no early-exit: AC wins earlier than has (≳48 needles on
	 * 1 MiB). Small batches use per-needle fss_count_raw (beats roller).
	 */
	if (k >= 48 && n >= 4096) {
		if (count_scan_ac(h, n, blob, offs, lens, k, overlap, out_counts))
			return;
		memset(out_counts, 0, k * sizeof(uint32_t));
	}

	uint8_t presence[32];
	int use_presence = (n > 0 && n < 256u * 1024u);
	if (use_presence) {
		memset(presence, 0, sizeof presence);
		for (size_t i = 0; i < n; i++) fss_presence_set(presence, h[i]);
	} else {
		memset(presence, 0xff, sizeof presence);
	}

	for (size_t i = 0; i < k; i++) {
		if (lens[i] == 0) continue;
		if ((size_t)lens[i] > n) continue;
		int ok = 1;
		if (use_presence) {
			for (uint32_t j = 0; j < lens[i]; j++) {
				if (!fss_presence_has(presence, blob[offs[i] + j])) {
					ok = 0;
					break;
				}
			}
		}
		if (!ok) continue;
		out_counts[i] = (uint32_t)fss_count_raw(h, n, blob + offs[i],
		                                        lens[i], overlap);
	}
}

void fss_count_batch(const uint8_t *h, size_t n, const uint8_t *const *nds,
                     const size_t *ms, size_t k, int overlap,
                     uint32_t *out_counts) {
	if (!out_counts || k == 0) return;
	size_t total = 0;
	for (size_t i = 0; i < k; i++) total += ms[i];
	uint8_t *blob = (uint8_t *)malloc(total ? total : 1);
	uint32_t *offs = (uint32_t *)malloc(k * sizeof(uint32_t));
	uint32_t *lens = (uint32_t *)malloc(k * sizeof(uint32_t));
	if (!blob || !offs || !lens) {
		free(blob);
		free(offs);
		free(lens);
		memset(out_counts, 0, k * sizeof(uint32_t));
		return;
	}
	size_t cur = 0;
	for (size_t i = 0; i < k; i++) {
		offs[i] = (uint32_t)cur;
		lens[i] = (uint32_t)ms[i];
		if (ms[i]) memcpy(blob + cur, nds[i], ms[i]);
		cur += ms[i];
	}
	fss_count_batch_blob(h, n, blob, offs, lens, k, overlap, out_counts);
	free(blob);
	free(offs);
	free(lens);
}
