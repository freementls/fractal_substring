/*
 * fcache — content-keyed computation cache (the fractal-caching core).
 *
 * A computation result store keyed by the *content* of a computation's
 * input. Callers hash their input (any granularity: token, field, line,
 * block), look up, and on a hit splice the precomputed result bytes instead
 * of recomputing. Layered caches at multiple granularities give the fractal
 * behavior: a hit at a coarse level skips the whole subtree of finer
 * computations; a miss falls through to finer levels and the result is
 * memoized upward.
 *
 * Design points:
 *  - open addressing, u64 keys, results in a bump arena (no per-entry
 *    malloc, no pointer chasing on the hot path)
 *  - graceful degradation: when the table or arena fills, inserts stop and
 *    lookups keep working — a saturated cache slows down, never breaks
 *  - an aux u64 per entry for side state (e.g. a transducer's next-state,
 *    a parsed numeric value) so (state, input) -> (output, state') style
 *    memoization of stateful passes works
 *  - single-threaded by design (built for single-core arbitrage)
 */
#ifndef FCACHE_H
#define FCACHE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint64_t key;      /* 0 = empty slot */
    uint32_t off, len; /* result bytes in arena */
    uint64_t aux;
} fc_slot;

typedef struct {
    fc_slot *slots;
    size_t mask, used, max_used;
    unsigned char *arena;
    size_t arena_cap, arena_used;
    uint64_t hits, misses, dropped;
    /* adaptive ROI gate: a level that isn't hitting is pure overhead, so it
     * measures its own window hit rate, bypasses itself for a stretch, then
     * re-probes (warm phases re-enable it automatically) */
    uint32_t win_lookups, bypass;
    uint64_t win_hits_base, gate_trips;
} fc_cache;

/* returns 1 when the cache should be consulted this call */
static inline int fc_gate_open(fc_cache *c) {
    if (c->bypass) { c->bypass--; return 0; }
    return 1;
}

/* call after every lookup (hit or miss) to run the window accounting */
static inline void fc_gate_tick(fc_cache *c) {
    if (++c->win_lookups == 8192) {
        uint64_t win_hits = c->hits - c->win_hits_base;
        if (win_hits < 8192 / 50) { c->bypass = 16384; c->gate_trips++; }
        c->win_lookups = 0;
        c->win_hits_base = c->hits;
    }
}

static inline uint64_t fc_mix64(uint64_t z) {
    z += 0x9e3779b97f4a7c15ULL;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* content hash: 8 bytes per step + mix; ~2.5 GB/s scalar, collision-safe
 * enough for memoization (verified by byte-identical output checks) */
static inline uint64_t fc_hash(const void *p, size_t n) {
    const unsigned char *s = (const unsigned char *)p;
    uint64_t h = 0x2545f4914f6cdd1dULL ^ (uint64_t)n;
    while (n >= 8) {
        uint64_t k;
        memcpy(&k, s, 8);
        h = fc_mix64(h ^ k);
        s += 8;
        n -= 8;
    }
    if (n) {
        uint64_t k = 0;
        memcpy(&k, s, n);
        h = fc_mix64(h ^ k ^ ((uint64_t)n << 56));
    }
    return h ? h : 1;
}

static inline fc_cache *fc_create(int slots_log2, size_t arena_bytes) {
    fc_cache *c = (fc_cache *)calloc(1, sizeof(fc_cache));
    if (!c) return NULL;
    size_t n = (size_t)1 << slots_log2;
    c->slots = (fc_slot *)calloc(n, sizeof(fc_slot));
    c->arena = (unsigned char *)malloc(arena_bytes);
    if (!c->slots || !c->arena) { free(c->slots); free(c->arena); free(c); return NULL; }
    c->mask = n - 1;
    c->max_used = n - (n >> 2); /* stop inserting at 75% load */
    c->arena_cap = arena_bytes;
    return c;
}

static inline void fc_free(fc_cache *c) {
    if (!c) return;
    free(c->slots);
    free(c->arena);
    free(c);
}

static inline fc_slot *fc_get(fc_cache *c, uint64_t key) {
    size_t i = key & c->mask;
    for (;;) {
        fc_slot *s = &c->slots[i];
        if (s->key == key) { c->hits++; return s; }
        if (s->key == 0) { c->misses++; return NULL; }
        i = (i + 1) & c->mask;
    }
}

static inline const unsigned char *fc_val(const fc_cache *c, const fc_slot *s) {
    return c->arena + s->off;
}

/* insert after a miss; returns NULL when saturated (caller just recomputes
 * next time — correctness never depends on an insert landing) */
static inline fc_slot *fc_put(fc_cache *c, uint64_t key, const void *val,
                              uint32_t len, uint64_t aux) {
    if (c->used >= c->max_used || c->arena_used + len > c->arena_cap) {
        c->dropped++;
        return NULL;
    }
    size_t i = key & c->mask;
    while (c->slots[i].key != 0) {
        if (c->slots[i].key == key) return &c->slots[i]; /* raced with self */
        i = (i + 1) & c->mask;
    }
    fc_slot *s = &c->slots[i];
    s->key = key;
    s->off = (uint32_t)c->arena_used;
    s->len = len;
    s->aux = aux;
    if (len) memcpy(c->arena + c->arena_used, val, len);
    c->arena_used += len;
    c->used++;
    return s;
}

/* ---- persistence: the table + arena are flat, so warm state survives
 * across runs (a log pipeline starts each day warm from yesterday) ---- */
#include <stdio.h>

static inline int fc_save(const fc_cache *c, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint64_t hdr[4] = {0xFCAC4E01u, c->mask + 1, c->used, c->arena_used};
    int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 &&
             fwrite(c->slots, sizeof(fc_slot), c->mask + 1, f) == c->mask + 1 &&
             (c->arena_used == 0 ||
              fwrite(c->arena, 1, c->arena_used, f) == c->arena_used);
    fclose(f);
    return ok ? 0 : -1;
}

/* load into an existing cache created with the same slots_log2; arena must
 * be large enough. Returns 0 on success, -1 on mismatch/missing (caller
 * just starts cold). */
static inline int fc_load(fc_cache *c, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t hdr[4];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != 0xFCAC4E01u ||
        hdr[1] != c->mask + 1 || hdr[3] > c->arena_cap) {
        fclose(f);
        return -1;
    }
    int ok = fread(c->slots, sizeof(fc_slot), c->mask + 1, f) == c->mask + 1 &&
             (hdr[3] == 0 || fread(c->arena, 1, hdr[3], f) == hdr[3]);
    fclose(f);
    if (!ok) {
        memset(c->slots, 0, (c->mask + 1) * sizeof(fc_slot));
        return -1;
    }
    c->used = hdr[2];
    c->arena_used = hdr[3];
    return 0;
}

#endif /* FCACHE_H */
