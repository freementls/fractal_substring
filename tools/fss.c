/*
 * tools/fss.c — CLI for find / count / has (hastok wire) / repeats.
 *
 * has mode (hastok-compatible):
 *   argv[1] ignored for subcommand; use: fss has <corpus-file>
 *   stdin  = u32 count, then count x (u32 len, len bytes)
 *   stdout = count bytes, 1 = present, 0 = absent
 */
#define _POSIX_C_SOURCE 200809L
#include "fss.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void *xmalloc(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) {
		fprintf(stderr, "fss: oom\n");
		exit(2);
	}
	return p;
}

static int read_exact(FILE *f, void *dst, size_t n) {
	return n == 0 || fread(dst, 1, n, f) == n;
}

static int read_u32(FILE *f, uint32_t *out) {
	unsigned char b[4];
	if (!read_exact(f, b, 4)) return 0;
	*out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
	       ((uint32_t)b[3] << 24);
	return 1;
}

static unsigned char *read_file(const char *path, size_t *len_out) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		fprintf(stderr, "fss: cannot open %s\n", path);
		exit(2);
	}
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		fprintf(stderr, "fss: seek failed\n");
		exit(2);
	}
	long sz = ftell(f);
	if (sz < 0) {
		fclose(f);
		exit(2);
	}
	rewind(f);
	unsigned char *buf = xmalloc((size_t)sz);
	if ((size_t)sz && fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		fprintf(stderr, "fss: short read\n");
		exit(2);
	}
	fclose(f);
	*len_out = (size_t)sz;
	return buf;
}

static int cmd_has(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: fss has <corpus-file>\n");
		return 2;
	}
	size_t corpus_len = 0;
	unsigned char *corpus = read_file(argv[1], &corpus_len);

	uint32_t count;
	if (!read_u32(stdin, &count)) {
		fprintf(stderr, "fss has: truncated header\n");
		return 2;
	}
	uint32_t *offs = xmalloc(count * sizeof(uint32_t));
	uint32_t *lens = xmalloc(count * sizeof(uint32_t));
	size_t blob_cap = 1u << 16, blob_used = 0;
	unsigned char *blob = xmalloc(blob_cap);
	unsigned char *found = xmalloc(count ? count : 1);

	for (uint32_t i = 0; i < count; i++) {
		uint32_t tl;
		if (!read_u32(stdin, &tl)) {
			fprintf(stderr, "fss has: truncated token %u\n", i);
			return 2;
		}
		if (tl > 4096u) {
			fprintf(stderr, "fss has: token too long\n");
			return 2;
		}
		while (blob_used + tl > blob_cap) {
			blob_cap *= 2;
			blob = realloc(blob, blob_cap);
			if (!blob) return 2;
		}
		if (!read_exact(stdin, blob + blob_used, tl)) {
			fprintf(stderr, "fss has: truncated body\n");
			return 2;
		}
		offs[i] = (uint32_t)blob_used;
		lens[i] = tl;
		blob_used += tl;
	}

	fss_has_batch_blob(corpus, corpus_len, blob, offs, lens, count, found);

	if (count && fwrite(found, 1, count, stdout) != count) {
		fprintf(stderr, "fss has: short write\n");
		return 2;
	}
	free(found);
	free(blob);
	free(offs);
	free(lens);
	free(corpus);
	return 0;
}

static int cmd_find(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: fss find <haystack-file> <needle>\n");
		return 2;
	}
	size_t n = 0;
	unsigned char *h = read_file(argv[1], &n);
	const unsigned char *nd = (const unsigned char *)argv[2];
	size_t m = strlen(argv[2]);
	ssize_t off = fss_find(h, n, nd, m);
	printf("%zd\n", (ssize_t)off);
	free(h);
	return 0;
}

static int cmd_count(int argc, char **argv) {
	if (argc < 3) {
		fprintf(stderr, "usage: fss count <haystack-file> <needle> [overlap]\n");
		return 2;
	}
	size_t n = 0;
	unsigned char *h = read_file(argv[1], &n);
	const unsigned char *nd = (const unsigned char *)argv[2];
	size_t m = strlen(argv[2]);
	int overlap = (argc >= 4 && argv[3][0] == '1') ? 1 : 0;
	size_t c = fss_count(h, n, nd, m, overlap);
	printf("%zu\n", c);
	free(h);
	return 0;
}

static int write_u32(FILE *f, uint32_t v) {
	unsigned char b[4] = {
	    (unsigned char)(v & 0xff),
	    (unsigned char)((v >> 8) & 0xff),
	    (unsigned char)((v >> 16) & 0xff),
	    (unsigned char)((v >> 24) & 0xff),
	};
	return fwrite(b, 1, 4, f) == 4;
}

static int cmd_count_batch(int argc, char **argv) {
	/* stdin: u32 k, k×(u32 len, bytes); stdout: k×u32 counts LE */
	if (argc < 2) {
		fprintf(stderr, "usage: fss count-batch <haystack-file>\n");
		return 2;
	}
	size_t corpus_len = 0;
	unsigned char *corpus = read_file(argv[1], &corpus_len);
	uint32_t count;
	if (!read_u32(stdin, &count)) {
		fprintf(stderr, "fss count-batch: truncated header\n");
		return 2;
	}
	uint32_t *offs = xmalloc(count * sizeof(uint32_t));
	uint32_t *lens = xmalloc(count * sizeof(uint32_t));
	size_t blob_cap = 1u << 16, blob_used = 0;
	unsigned char *blob = xmalloc(blob_cap);
	uint32_t *outc = xmalloc((count ? count : 1) * sizeof(uint32_t));

	for (uint32_t i = 0; i < count; i++) {
		uint32_t tl;
		if (!read_u32(stdin, &tl)) {
			fprintf(stderr, "fss count-batch: truncated token %u\n", i);
			return 2;
		}
		while (blob_used + tl > blob_cap) {
			blob_cap *= 2;
			blob = realloc(blob, blob_cap);
			if (!blob) return 2;
		}
		if (!read_exact(stdin, blob + blob_used, tl)) {
			fprintf(stderr, "fss count-batch: truncated body\n");
			return 2;
		}
		offs[i] = (uint32_t)blob_used;
		lens[i] = tl;
		blob_used += tl;
	}
	fss_count_batch_blob(corpus, corpus_len, blob, offs, lens, count, 0, outc);
	for (uint32_t i = 0; i < count; i++) {
		if (!write_u32(stdout, outc[i])) {
			fprintf(stderr, "fss count-batch: short write\n");
			return 2;
		}
	}
	free(outc);
	free(blob);
	free(offs);
	free(lens);
	free(corpus);
	return 0;
}

static int cmd_repeats(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr, "usage: fss repeats <haystack-file> [min] [max] [topk]\n");
		return 2;
	}
	size_t n = 0;
	unsigned char *h = read_file(argv[1], &n);
	fss_repeat_opts o = {0};
	o.min_len = argc >= 3 ? (size_t)atoi(argv[2]) : 4;
	o.max_len = argc >= 4 ? (size_t)atoi(argv[3]) : 128;
	o.min_count = 2;
	o.top_k = argc >= 5 ? (size_t)atoi(argv[4]) : 16;
	fss_repeat out[64];
	size_t nr = fss_repeats(h, n, &o, out, 64);
	/* Machine-readable: estLin \t count \t len \t base64(needle) */
	static const char b64t[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	for (size_t i = 0; i < nr; i++) {
		printf("%d\t%u\t%zu\t", out[i].est_lin, out[i].count, out[i].len);
		const unsigned char *p = out[i].p;
		size_t L = out[i].len;
		for (size_t j = 0; j < L; j += 3) {
			unsigned a = p[j];
			unsigned b = (j + 1 < L) ? p[j + 1] : 0;
			unsigned c = (j + 2 < L) ? p[j + 2] : 0;
			unsigned triple = (a << 16) | (b << 8) | c;
			fputc(b64t[(triple >> 18) & 63], stdout);
			fputc(b64t[(triple >> 12) & 63], stdout);
			fputc((j + 1 < L) ? b64t[(triple >> 6) & 63] : '=', stdout);
			fputc((j + 2 < L) ? b64t[triple & 63] : '=', stdout);
		}
		fputc('\n', stdout);
	}
	free(h);
	return 0;
}

int main(int argc, char **argv) {
	if (argc < 2) {
		fprintf(stderr,
		        "usage: fss <find|count|count-batch|has|repeats> ...\n"
		        "  has <corpus>         — hastok-compatible batch presence\n"
		        "  count-batch <hay>    — stdin needles → stdout u32 counts\n");
		return 2;
	}
	const char *cmd = argv[1];
	if (strcmp(cmd, "has") == 0) return cmd_has(argc - 1, argv + 1);
	if (strcmp(cmd, "find") == 0) return cmd_find(argc - 1, argv + 1);
	if (strcmp(cmd, "count") == 0) return cmd_count(argc - 1, argv + 1);
	if (strcmp(cmd, "count-batch") == 0) return cmd_count_batch(argc - 1, argv + 1);
	if (strcmp(cmd, "repeats") == 0) return cmd_repeats(argc - 1, argv + 1);
	/* Bare hastok drop-in: `fss <corpus>` acts as has */
	if (argc == 2) return cmd_has(argc, argv);
	fprintf(stderr, "fss: unknown command %s\n", cmd);
	return 2;
}
