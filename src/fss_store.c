/*
 * fss_store.c — fmem intern + fcache memo + attached profile.
 */
#include "fss_internal.h"

#include "fcache.h"
#include "fmem.h"

#include <stdlib.h>
#include <string.h>

struct fss_store {
	fm_store *mem;
	fc_cache *cache;
	fss_profile **profiles; /* parallel to fm objs, indexed by handle */
	size_t profiles_cap;
	/* Pinned materialization for repeats_in pointer validity */
	uint8_t *pin;
	size_t pin_n;
	fss_handle pin_h;
};

fss_store *fss_store_create(void) {
	fss_store *s = (fss_store *)calloc(1, sizeof(*s));
	if (!s) return NULL;
	s->mem = fm_create();
	s->cache = fc_create(16, 4u << 20); /* 64K slots, 4 MiB arena */
	s->pin_h = FSS_NULL;
	if (!s->mem || !s->cache) {
		if (s->mem) fm_destroy(s->mem);
		if (s->cache) fc_free(s->cache);
		free(s);
		return NULL;
	}
	return s;
}

void fss_store_free(fss_store *s) {
	if (!s) return;
	for (size_t i = 0; i < s->profiles_cap; i++)
		fss_profile_free(s->profiles[i]);
	free(s->profiles);
	free(s->pin);
	fm_destroy(s->mem);
	fc_free(s->cache);
	free(s);
}

static int ensure_profile_slot(fss_store *s, fss_handle h) {
	if ((size_t)h < s->profiles_cap) return 0;
	size_t nc = (size_t)h + 1;
	if (nc < 16) nc = 16;
	while (nc <= (size_t)h) nc *= 2;
	fss_profile **np =
	    (fss_profile **)realloc(s->profiles, nc * sizeof(fss_profile *));
	if (!np) return -1;
	for (size_t i = s->profiles_cap; i < nc; i++) np[i] = NULL;
	s->profiles = np;
	s->profiles_cap = nc;
	return 0;
}

fss_handle fss_intern(fss_store *s, const void *p, size_t n) {
	if (!s) return FSS_NULL;
	fm_handle h = fm_intern(s->mem, p, n);
	if (h == FM_NULL) return FSS_NULL;
	ensure_profile_slot(s, (fss_handle)h);
	return (fss_handle)h;
}

void fss_release(fss_store *s, fss_handle h) {
	if (!s || h == FSS_NULL) return;
	if ((size_t)h < s->profiles_cap) {
		fss_profile_free(s->profiles[h]);
		s->profiles[h] = NULL;
	}
	fm_release(s->mem, (fm_handle)h);
}

static fss_profile *get_or_build_profile(fss_store *s, fss_handle h) {
	if ((size_t)h >= s->profiles_cap || !s->profiles[h]) {
		ensure_profile_slot(s, h);
		uint64_t len = fm_len(s->mem, (fm_handle)h);
		uint8_t *buf = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
		if (!buf) return NULL;
		fm_read(s->mem, (fm_handle)h, 0, len, buf);
		int budget = len >= 4096 ? 2 : 1;
		s->profiles[h] = fss_profile_build(buf, (size_t)len, budget);
		free(buf);
	}
	return s->profiles[h];
}

static uint8_t *materialize(fss_store *s, fss_handle h, size_t *n_out) {
	uint64_t len = fm_len(s->mem, (fm_handle)h);
	uint8_t *buf = (uint8_t *)malloc((size_t)len ? (size_t)len : 1);
	if (!buf) return NULL;
	fm_read(s->mem, (fm_handle)h, 0, len, buf);
	*n_out = (size_t)len;
	return buf;
}

static uint8_t *pin_hay(fss_store *s, fss_handle h, size_t *n_out) {
	if (s->pin && s->pin_h == h) {
		*n_out = s->pin_n;
		return s->pin;
	}
	free(s->pin);
	s->pin = NULL;
	s->pin_h = FSS_NULL;
	s->pin_n = 0;
	uint8_t *buf = materialize(s, h, n_out);
	if (!buf) return NULL;
	s->pin = buf;
	s->pin_n = *n_out;
	s->pin_h = h;
	return buf;
}

size_t fss_repeats_in(fss_store *s, fss_handle hay, const fss_repeat_opts *o,
                      fss_repeat *out, size_t out_cap) {
	if (!s || hay == FSS_NULL || !o || !out || out_cap == 0) return 0;

	uint64_t hh = fm_content_hash(s->mem, (fm_handle)hay);
	uint64_t opts_h = fc_mix64((uint64_t)o->min_len);
	opts_h = fc_mix64(opts_h ^ (uint64_t)o->max_len);
	opts_h = fc_mix64(opts_h ^ (uint64_t)o->min_count);
	opts_h = fc_mix64(opts_h ^ (uint64_t)o->top_k);
	opts_h = fc_mix64(opts_h ^ (uint64_t)(uint32_t)o->marker_len_base);
	uint64_t key = fc_mix64(hh ^ opts_h ^ 0x5EED5EEDULL);
	if (!key) key = 1;

	size_t n = 0;
	uint8_t *buf = pin_hay(s, hay, &n);
	if (!buf) return 0;

	/* Packed memo: aux = count; arena = [off u64][len u32][count u32][est i32] * k */
	if (fc_gate_open(s->cache)) {
		fc_slot *slot = fc_get(s->cache, key);
		fc_gate_tick(s->cache);
		if (slot && slot->len > 0 && slot->aux > 0) {
			size_t k = (size_t)slot->aux;
			if (k > out_cap) k = out_cap;
			const unsigned char *p = fc_val(s->cache, slot);
			size_t rec = 8 + 4 + 4 + 4;
			if (slot->len < k * rec) return 0;
			size_t written = 0;
			for (size_t i = 0; i < k && written < out_cap; i++) {
				uint64_t off;
				uint32_t len, count;
				int32_t est;
				memcpy(&off, p + i * rec, 8);
				memcpy(&len, p + i * rec + 8, 4);
				memcpy(&count, p + i * rec + 12, 4);
				memcpy(&est, p + i * rec + 16, 4);
				if (off + len > n) continue;
				out[written].p = buf + (size_t)off;
				out[written].len = len;
				out[written].count = count;
				out[written].est_lin = est;
				written++;
			}
			return written;
		}
	} else {
		fc_gate_tick(s->cache);
	}

	fss_profile *prof = get_or_build_profile(s, hay);
	size_t nr = fss_repeats_ex(buf, n, o, out, out_cap, prof);
	if (nr == 0) return 0;

	size_t rec = 8 + 4 + 4 + 4;
	size_t bytes = nr * rec;
	unsigned char *pack = (unsigned char *)malloc(bytes);
	if (pack) {
		for (size_t i = 0; i < nr; i++) {
			uint64_t off = (uint64_t)(out[i].p - buf);
			uint32_t len = (uint32_t)out[i].len;
			uint32_t count = out[i].count;
			int32_t est = out[i].est_lin;
			memcpy(pack + i * rec, &off, 8);
			memcpy(pack + i * rec + 8, &len, 4);
			memcpy(pack + i * rec + 12, &count, 4);
			memcpy(pack + i * rec + 16, &est, 4);
		}
		fc_put(s->cache, key, pack, (uint32_t)bytes, (uint64_t)nr);
		free(pack);
	}
	return nr;
}

ssize_t fss_find_in(fss_store *s, fss_handle hay, const uint8_t *nd,
                    size_t m) {
	if (!s || hay == FSS_NULL) return -1;

	/* fcache: key = mix(hay_content_hash, needle_hash) -> offset+1 or 0 */
	uint64_t hh = fm_content_hash(s->mem, (fm_handle)hay);
	uint64_t nh = fss_hash_bytes(nd, m);
	uint64_t key = fc_mix64(hh ^ fc_mix64(nh) ^ fc_mix64(m));
	if (!key) key = 1;

	if (fc_gate_open(s->cache)) {
		fc_slot *slot = fc_get(s->cache, key);
		fc_gate_tick(s->cache);
		if (slot) {
			/* aux stores offset+1; 0 means not found */
			if (slot->aux == 0) return -1;
			return (ssize_t)(slot->aux - 1);
		}
	} else {
		fc_gate_tick(s->cache);
	}

	size_t n = 0;
	uint8_t *buf = materialize(s, hay, &n);
	if (!buf) return -1;
	fss_profile *prof = get_or_build_profile(s, hay);
	ssize_t r = fss_find_ex(buf, n, nd, m, prof);
	free(buf);

	uint64_t aux = (r < 0) ? 0 : (uint64_t)r + 1;
	fc_put(s->cache, key, NULL, 0, aux);
	return r;
}

size_t fss_count_in(fss_store *s, fss_handle hay, const uint8_t *nd, size_t m,
                    int overlap) {
	if (!s || hay == FSS_NULL) return 0;

	uint64_t hh = fm_content_hash(s->mem, (fm_handle)hay);
	uint64_t nh = fss_hash_bytes(nd, m);
	uint64_t key =
	    fc_mix64(hh ^ fc_mix64(nh) ^ fc_mix64(m) ^ fc_mix64((uint64_t)overlap + 0xC0u));
	if (!key) key = 1;

	if (fc_gate_open(s->cache)) {
		fc_slot *slot = fc_get(s->cache, key);
		fc_gate_tick(s->cache);
		if (slot) return (size_t)slot->aux;
	} else {
		fc_gate_tick(s->cache);
	}

	size_t n = 0;
	uint8_t *buf = materialize(s, hay, &n);
	if (!buf) return 0;
	fss_profile *prof = get_or_build_profile(s, hay);
	size_t r = fss_count_ex(buf, n, nd, m, overlap, prof);
	free(buf);
	fc_put(s->cache, key, NULL, 0, (uint64_t)r);
	return r;
}

void fss_has_batch_in(fss_store *s, fss_handle hay, const uint8_t *const *nds,
                      const size_t *ms, size_t k, uint8_t *out_bits) {
	if (!out_bits) return;
	if (!s || hay == FSS_NULL || !nds || !ms) {
		if (k) memset(out_bits, 0, k);
		return;
	}
	if (k == 0) return;

	uint64_t hh = fm_content_hash(s->mem, (fm_handle)hay);
	uint64_t nh = fc_mix64((uint64_t)k ^ 0xBA7CULL);
	for (size_t i = 0; i < k; i++) {
		nh = fc_mix64(nh ^ fss_hash_bytes(nds[i], ms[i]) ^ fc_mix64(ms[i]));
	}
	uint64_t key = fc_mix64(hh ^ nh ^ 0xBABAULL);
	if (!key) key = 1;

	if (fc_gate_open(s->cache)) {
		fc_slot *slot = fc_get(s->cache, key);
		fc_gate_tick(s->cache);
		if (slot && slot->len == k) {
			memcpy(out_bits, fc_val(s->cache, slot), k);
			return;
		}
	} else {
		fc_gate_tick(s->cache);
	}

	size_t n = 0;
	uint8_t *buf = pin_hay(s, hay, &n);
	if (!buf) {
		memset(out_bits, 0, k);
		return;
	}
	fss_has_batch(buf, n, nds, ms, k, out_bits);
	fc_put(s->cache, key, out_bits, (uint32_t)k, (uint64_t)k);
}
