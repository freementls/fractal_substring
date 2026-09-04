/*
 * fmem — fractal memory: content-deduplicated in-RAM object store.
 *
 * fractal_zip avoids storing repeated bytes on disk; fcache avoids
 * computing over repeated inputs; fmem avoids holding repeated bytes in
 * RAM. Two levels, each chosen by the memcensus verdict
 * (benchmarks/.fcache_bench/mc_*.json):
 *
 *   L1  whole-object intern: identical buffers share one handle
 *       (refcounted); catches 17-28% on real corpora for one hash.
 *   L2  content-defined chunks (gear hash, min 1 KiB / avg 4 KiB /
 *       max 16 KiB): objects become chunk-reference vectors over a shared
 *       chunk store. CDC beats fixed blocks decisively on reordered data
 *       (2.31x vs 1.53x on the slices set) because boundaries realign.
 *
 * ROI gate (engine property, mirrors fcache's): the store tracks the chunk
 * hit rate over a sliding window; when dedup stops paying (compressed or
 * unique data), new objects are stored raw — one memcpy, no hashing, no
 * table pressure — and the store re-probes periodically so dedup-friendly
 * phases re-enable chunking. The memcensus control set (.fz outputs,
 * 1.00x, net-negative after metadata) is why this exists.
 *
 * Arena pointers are stable (chunks live in fixed-size arena segments,
 * never realloc'd), so fm_chunk() pointers remain valid for the store's
 * lifetime. Chunk space of fully released objects is reclaimed on the
 * fm_save/fm_load round trip (compaction skips dead chunks), not inline.
 */
#ifndef FMEM_H
#define FMEM_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fcache.h" /* fc_mix64 / fc_hash */

#define FM_SEG_BYTES (16u << 20)  /* arena segment; chunks never span */
#define FM_CDC_MIN 1024
#define FM_CDC_AVG_MASK 0xFFF     /* ~4 KiB average */
#define FM_CDC_MAX 16384
#define FM_GATE_WINDOW 4096       /* chunks per ROI window */
#define FM_GATE_MIN_HITS 82       /* ~2% of window */
#define FM_GATE_BYPASS 16384      /* chunks stored raw before re-probe */
#define FM_GATE_WARMUP 8192       /* no gating before the store has seen this many chunks */

typedef uint32_t fm_handle;
#define FM_NULL ((fm_handle)0xFFFFFFFFu)

typedef struct {
    uint64_t hash;      /* 0 = empty slot in the index */
    uint32_t seg, off;  /* location in arena */
    uint32_t len;
    uint32_t refs;
} fm_chunk_ent;

typedef struct {
    uint64_t hash;      /* whole-object content hash (also the L1 key) */
    uint64_t len;
    uint32_t refs;
    uint32_t nchunks;   /* 0 => raw object */
    uint32_t *chunk_ix; /* chunk entry indices, or NULL for raw */
    uint64_t *cum;      /* cumulative end offsets per chunk (for fm_read) */
    unsigned char *raw; /* raw storage when gated */
} fm_obj;

typedef struct {
    /* chunk store */
    fm_chunk_ent *chunks;
    size_t chunks_used, chunks_cap;
    size_t *cindex;             /* open-addressed: entry index+1, 0 empty */
    size_t cindex_mask;
    unsigned char **segs;
    size_t nsegs, segs_cap, seg_used; /* seg_used = bytes used in last seg */
    /* objects */
    fm_obj *objs;
    size_t objs_used, objs_cap;
    size_t *oindex;             /* L1: open-addressed obj index+1 */
    size_t oindex_mask;
    /* stats */
    uint64_t raw_bytes;         /* total bytes interned (logical) */
    uint64_t l1_hits, chunk_hits, chunk_misses;
    uint64_t gate_raw_bytes, gate_trips;
    /* ROI gate */
    uint32_t win_chunks, bypass_chunks;
    uint64_t win_hits_base;
    /* cdc */
    uint64_t gear[256];
} fm_store;

/* ---------- internal: tables ---------- */

static inline void fm__cindex_insert(fm_store *m, uint64_t h, size_t ent) {
    size_t i = h & m->cindex_mask;
    while (m->cindex[i]) i = (i + 1) & m->cindex_mask;
    m->cindex[i] = ent + 1;
}

static inline void fm__cindex_grow(fm_store *m) {
    size_t newn = (m->cindex_mask + 1) * 2;
    free(m->cindex);
    m->cindex = (size_t *)calloc(newn, sizeof(size_t));
    m->cindex_mask = newn - 1;
    for (size_t e = 0; e < m->chunks_used; ++e)
        if (m->chunks[e].hash) fm__cindex_insert(m, m->chunks[e].hash, e);
}

static inline void fm__oindex_insert(fm_store *m, uint64_t h, size_t ent) {
    size_t i = h & m->oindex_mask;
    while (m->oindex[i]) i = (i + 1) & m->oindex_mask;
    m->oindex[i] = ent + 1;
}

static inline void fm__oindex_grow(fm_store *m) {
    size_t newn = (m->oindex_mask + 1) * 2;
    free(m->oindex);
    m->oindex = (size_t *)calloc(newn, sizeof(size_t));
    m->oindex_mask = newn - 1;
    for (size_t e = 0; e < m->objs_used; ++e)
        if (m->objs[e].refs) fm__oindex_insert(m, m->objs[e].hash, e);
}

static inline unsigned char *fm__arena_alloc(fm_store *m, uint32_t len,
                                             uint32_t *seg, uint32_t *off) {
    if (m->nsegs == 0 || m->seg_used + len > FM_SEG_BYTES) {
        if (m->nsegs == m->segs_cap) {
            m->segs_cap = m->segs_cap ? m->segs_cap * 2 : 16;
            m->segs = (unsigned char **)realloc(m->segs,
                                                m->segs_cap * sizeof(void *));
        }
        m->segs[m->nsegs++] = (unsigned char *)malloc(FM_SEG_BYTES);
        m->seg_used = 0;
    }
    *seg = (uint32_t)(m->nsegs - 1);
    *off = (uint32_t)m->seg_used;
    unsigned char *p = m->segs[m->nsegs - 1] + m->seg_used;
    m->seg_used += len;
    return p;
}

/* find-or-insert one chunk; returns chunk entry index.
 * Hits are memcmp-verified: a memory store must never serve wrong bytes on
 * a hash collision, so equality means content equality (the compare costs
 * one pass, i.e. no more than the memcpy a hit avoids).
 * (Optimize-loop note: a 4-lane bulk hash for chunk keys was tried and
 * reverted — no measured win; intern wall is CDC + copies, not hashing.) */
static inline size_t fm__chunk_intern(fm_store *m, const unsigned char *p,
                                      uint32_t len) {
    uint64_t h = fc_hash(p, len) ^ fc_mix64(len);
    if (!h) h = 1;
    size_t i = h & m->cindex_mask;
    while (m->cindex[i]) {
        size_t e = m->cindex[i] - 1;
        if (m->chunks[e].hash == h && m->chunks[e].len == len &&
            !memcmp(m->segs[m->chunks[e].seg] + m->chunks[e].off, p, len)) {
            m->chunks[e].refs++;
            m->chunk_hits++;
            return e;
        }
        i = (i + 1) & m->cindex_mask;
    }
    m->chunk_misses++;
    if (m->chunks_used == m->chunks_cap) {
        m->chunks_cap = m->chunks_cap ? m->chunks_cap * 2 : 4096;
        m->chunks = (fm_chunk_ent *)realloc(m->chunks,
                                            m->chunks_cap * sizeof(fm_chunk_ent));
    }
    if (m->chunks_used * 10 >= (m->cindex_mask + 1) * 7) fm__cindex_grow(m);
    size_t e = m->chunks_used++;
    fm_chunk_ent *ce = &m->chunks[e];
    ce->hash = h;
    ce->len = len;
    ce->refs = 1;
    unsigned char *dst = fm__arena_alloc(m, len, &ce->seg, &ce->off);
    memcpy(dst, p, len);
    fm__cindex_insert(m, h, e);
    return e;
}

/* ---------- API ---------- */

static inline fm_store *fm_create(void) {
    fm_store *m = (fm_store *)calloc(1, sizeof(fm_store));
    m->cindex_mask = (1u << 16) - 1;
    m->cindex = (size_t *)calloc(m->cindex_mask + 1, sizeof(size_t));
    m->oindex_mask = (1u << 12) - 1;
    m->oindex = (size_t *)calloc(m->oindex_mask + 1, sizeof(size_t));
    uint64_t seed = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 256; ++i) m->gear[i] = seed = fc_mix64(seed + i);
    return m;
}

static inline void fm_destroy(fm_store *m) {
    if (!m) return;
    for (size_t i = 0; i < m->objs_used; ++i) {
        free(m->objs[i].chunk_ix);
        free(m->objs[i].cum);
        free(m->objs[i].raw);
    }
    for (size_t i = 0; i < m->nsegs; ++i) free(m->segs[i]);
    free(m->segs);
    free(m->chunks);
    free(m->cindex);
    free(m->objs);
    free(m->oindex);
    free(m);
}

/* content-equality check for L1 hits (collision safety) */
static inline int fm__obj_equal(fm_store *m, fm_obj *o,
                                const unsigned char *p, size_t n) {
    if (o->len != n) return 0;
    if (!o->nchunks) return n == 0 || !memcmp(o->raw, p, n);
    size_t off = 0;
    for (uint32_t c = 0; c < o->nchunks; ++c) {
        fm_chunk_ent *ce = &m->chunks[o->chunk_ix[c]];
        if (memcmp(m->segs[ce->seg] + ce->off, p + off, ce->len)) return 0;
        off += ce->len;
    }
    return 1;
}

static inline fm_handle fm_intern(fm_store *m, const void *ptr, size_t n) {
    const unsigned char *p = (const unsigned char *)ptr;
    uint64_t oh = fc_hash(p, n) ^ fc_mix64(n);
    if (!oh) oh = 1;
    m->raw_bytes += n;

    /* L1: whole-object */
    size_t i = oh & m->oindex_mask;
    while (m->oindex[i]) {
        size_t e = m->oindex[i] - 1;
        if (m->objs[e].hash == oh && m->objs[e].refs &&
            fm__obj_equal(m, &m->objs[e], p, n)) {
            m->objs[e].refs++;
            m->l1_hits++;
            return (fm_handle)e;
        }
        i = (i + 1) & m->oindex_mask;
    }

    if (m->objs_used == m->objs_cap) {
        m->objs_cap = m->objs_cap ? m->objs_cap * 2 : 1024;
        m->objs = (fm_obj *)realloc(m->objs, m->objs_cap * sizeof(fm_obj));
    }
    if (m->objs_used * 10 >= (m->oindex_mask + 1) * 7) fm__oindex_grow(m);
    size_t e = m->objs_used++;
    fm_obj *o = &m->objs[e];
    memset(o, 0, sizeof *o);
    o->hash = oh;
    o->len = n;
    o->refs = 1;

    /* ROI gate: when chunk dedup isn't paying, store raw (one memcpy, no
     * hashing); re-probe after FM_GATE_BYPASS chunks' worth of bytes */
    uint64_t seen = m->chunk_hits + m->chunk_misses;
    if (m->bypass_chunks && seen >= FM_GATE_WARMUP) {
        size_t approx_chunks = n / (FM_CDC_AVG_MASK + 1) + 1;
        m->bypass_chunks -= approx_chunks < m->bypass_chunks
                                ? (uint32_t)approx_chunks
                                : m->bypass_chunks;
        o->raw = (unsigned char *)malloc(n ? n : 1);
        memcpy(o->raw, p, n);
        m->gate_raw_bytes += n;
        fm__oindex_insert(m, oh, e);
        return (fm_handle)e;
    }

    /* L2: content-defined chunking */
    uint32_t cap = 16, cnt = 0;
    uint32_t *ix = (uint32_t *)malloc(cap * sizeof(uint32_t));
    uint64_t *cum = (uint64_t *)malloc(cap * sizeof(uint64_t));
    size_t beg = 0;
    uint64_t gh = 0;
    for (size_t pos = 0; pos <= n; ++pos) {
        size_t len = pos - beg;
        int cut = (pos == n && len > 0) ||
                  (len >= FM_CDC_MIN && (gh & FM_CDC_AVG_MASK) == 0) ||
                  len >= FM_CDC_MAX;
        if (cut) {
            if (cnt == cap) {
                cap *= 2;
                ix = (uint32_t *)realloc(ix, cap * sizeof(uint32_t));
                cum = (uint64_t *)realloc(cum, cap * sizeof(uint64_t));
            }
            ix[cnt] = (uint32_t)fm__chunk_intern(m, p + beg, (uint32_t)len);
            cum[cnt] = pos;
            cnt++;
            beg = pos;
            gh = 0;

            /* window accounting */
            if (++m->win_chunks == FM_GATE_WINDOW) {
                uint64_t wh = m->chunk_hits - m->win_hits_base;
                if (wh < FM_GATE_MIN_HITS &&
                    m->chunk_hits + m->chunk_misses >= FM_GATE_WARMUP) {
                    m->bypass_chunks = FM_GATE_BYPASS;
                    m->gate_trips++;
                }
                m->win_chunks = 0;
                m->win_hits_base = m->chunk_hits;
            }
        }
        if (pos < n) gh = (gh << 1) + m->gear[p[pos]];
    }
    o->nchunks = cnt;
    o->chunk_ix = ix;
    o->cum = cum;
    fm__oindex_insert(m, oh, e);
    return (fm_handle)e;
}

static inline uint64_t fm_len(fm_store *m, fm_handle h) {
    return m->objs[h].len;
}

static inline uint64_t fm_content_hash(fm_store *m, fm_handle h) {
    return m->objs[h].hash;
}

static inline uint32_t fm_chunk_count(fm_store *m, fm_handle h) {
    return m->objs[h].nchunks; /* 0 for raw objects */
}

/* pointer + length of chunk c (stable for the store's lifetime) */
static inline const unsigned char *fm_chunk(fm_store *m, fm_handle h,
                                            uint32_t c, uint32_t *len) {
    fm_obj *o = &m->objs[h];
    if (!o->nchunks) { /* raw object: pretend it is one chunk */
        *len = (uint32_t)o->len;
        return o->raw;
    }
    fm_chunk_ent *ce = &m->chunks[o->chunk_ix[c]];
    *len = ce->len;
    return m->segs[ce->seg] + ce->off;
}

/* per-chunk content hash (the composition key for fcache clients) */
static inline uint64_t fm_chunk_hash(fm_store *m, fm_handle h, uint32_t c) {
    fm_obj *o = &m->objs[h];
    if (!o->nchunks) return o->hash;
    return m->chunks[o->chunk_ix[c]].hash;
}

/* copy out an arbitrary window (binary search + splice) */
static inline void fm_read(fm_store *m, fm_handle h, uint64_t off,
                           uint64_t len, void *dst) {
    fm_obj *o = &m->objs[h];
    unsigned char *d = (unsigned char *)dst;
    if (!o->nchunks) {
        memcpy(d, o->raw + off, len);
        return;
    }
    uint32_t lo = 0, hi = o->nchunks - 1;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (o->cum[mid] <= off) lo = mid + 1;
        else hi = mid;
    }
    uint64_t cbeg = lo ? o->cum[lo - 1] : 0;
    while (len) {
        fm_chunk_ent *ce = &m->chunks[o->chunk_ix[lo]];
        uint64_t skip = off - cbeg;
        uint64_t take = ce->len - skip;
        if (take > len) take = len;
        memcpy(d, m->segs[ce->seg] + ce->off + skip, take);
        d += take;
        off += take;
        len -= take;
        cbeg = o->cum[lo];
        lo++;
    }
}

static inline void fm_release(fm_store *m, fm_handle h) {
    fm_obj *o = &m->objs[h];
    if (!o->refs || --o->refs) return;
    for (uint32_t c = 0; c < o->nchunks; ++c)
        m->chunks[o->chunk_ix[c]].refs--;   /* space reclaimed on save/load */
    free(o->chunk_ix);
    free(o->cum);
    free(o->raw);
    o->chunk_ix = NULL;
    o->cum = NULL;
    o->raw = NULL;
}

/* bytes actually resident (unique chunk bytes + raw objects + tables) */
static inline uint64_t fm_resident_bytes(fm_store *m) {
    uint64_t arena = m->nsegs ? (m->nsegs - 1) * (uint64_t)FM_SEG_BYTES +
                                m->seg_used
                              : 0;
    uint64_t tables = (m->cindex_mask + 1) * sizeof(size_t) +
                      (m->oindex_mask + 1) * sizeof(size_t) +
                      m->chunks_used * sizeof(fm_chunk_ent) +
                      m->objs_used * sizeof(fm_obj);
    uint64_t refs = 0;
    for (size_t i = 0; i < m->objs_used; ++i)
        refs += m->objs[i].nchunks * 12ull;
    return arena + tables + refs + m->gate_raw_bytes;
}

static inline void fm_stats_json(fm_store *m, FILE *out) {
    fprintf(out,
        "{\"raw_bytes\":%llu,\"resident_bytes\":%llu,\"objects\":%zu,"
        "\"l1_hits\":%llu,\"chunks\":%zu,\"chunk_hits\":%llu,"
        "\"chunk_misses\":%llu,\"gate_raw_bytes\":%llu,\"gate_trips\":%llu,"
        "\"dedup_ratio\":%.3f}",
        (unsigned long long)m->raw_bytes,
        (unsigned long long)fm_resident_bytes(m), m->objs_used,
        (unsigned long long)m->l1_hits, m->chunks_used,
        (unsigned long long)m->chunk_hits,
        (unsigned long long)m->chunk_misses,
        (unsigned long long)m->gate_raw_bytes,
        (unsigned long long)m->gate_trips,
        fm_resident_bytes(m) ? (double)m->raw_bytes /
                               (double)fm_resident_bytes(m)
                             : 0.0);
}

/* ---------- persistence (compacts: dead chunks/objects dropped) ---------- */
#define FM_MAGIC 0xF4E401u

static inline int fm_save(fm_store *m, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint64_t live_objs = 0;
    for (size_t i = 0; i < m->objs_used; ++i)
        if (m->objs[i].refs) live_objs++;
    /* remap live chunks to a compact id space */
    uint32_t *remap = (uint32_t *)malloc(m->chunks_used * sizeof(uint32_t));
    uint64_t live_chunks = 0, live_bytes = 0;
    for (size_t e = 0; e < m->chunks_used; ++e) {
        if (m->chunks[e].refs) {
            remap[e] = (uint32_t)live_chunks++;
            live_bytes += m->chunks[e].len;
        } else remap[e] = 0xFFFFFFFFu;
    }
    uint64_t hdr[4] = {FM_MAGIC, live_chunks, live_objs, live_bytes};
    fwrite(hdr, sizeof hdr, 1, f);
    for (size_t e = 0; e < m->chunks_used; ++e) {
        if (!m->chunks[e].refs) continue;
        uint64_t meta[2] = {m->chunks[e].hash, m->chunks[e].len};
        fwrite(meta, sizeof meta, 1, f);
        fwrite(m->segs[m->chunks[e].seg] + m->chunks[e].off, 1,
               m->chunks[e].len, f);
    }
    for (size_t i = 0; i < m->objs_used; ++i) {
        fm_obj *o = &m->objs[i];
        if (!o->refs) continue;
        uint64_t meta[4] = {o->hash, o->len, o->refs, o->nchunks};
        fwrite(meta, sizeof meta, 1, f);
        if (o->nchunks) {
            for (uint32_t c = 0; c < o->nchunks; ++c) {
                uint32_t r = remap[o->chunk_ix[c]];
                fwrite(&r, sizeof r, 1, f);
            }
            fwrite(o->cum, sizeof(uint64_t), o->nchunks, f);
        } else if (o->len) {
            fwrite(o->raw, 1, o->len, f);
        }
    }
    free(remap);
    int rc = ferror(f) ? -1 : 0;
    fclose(f);
    return rc;
}

/* load into a fresh store created by fm_create(); handles are renumbered */
static inline int fm_load(fm_store *m, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint64_t hdr[4];
    if (fread(hdr, sizeof hdr, 1, f) != 1 || hdr[0] != FM_MAGIC) {
        fclose(f);
        return -1;
    }
    uint64_t nchunks = hdr[1], nobjs = hdr[2];
    uint32_t *cmap = (uint32_t *)malloc(nchunks * sizeof(uint32_t));
    unsigned char *tmp = (unsigned char *)malloc(FM_CDC_MAX);
    for (uint64_t e = 0; e < nchunks; ++e) {
        uint64_t meta[2];
        if (fread(meta, sizeof meta, 1, f) != 1) goto bad;
        uint32_t len = (uint32_t)meta[1];
        if (len > FM_CDC_MAX || fread(tmp, 1, len, f) != len) goto bad;
        /* re-intern: also merges with anything already in the store */
        cmap[e] = (uint32_t)fm__chunk_intern(m, tmp, len);
    }
    for (uint64_t i = 0; i < nobjs; ++i) {
        uint64_t meta[4];
        if (fread(meta, sizeof meta, 1, f) != 1) goto bad;
        if (m->objs_used == m->objs_cap) {
            m->objs_cap = m->objs_cap ? m->objs_cap * 2 : 1024;
            m->objs = (fm_obj *)realloc(m->objs, m->objs_cap * sizeof(fm_obj));
        }
        if (m->objs_used * 10 >= (m->oindex_mask + 1) * 7) fm__oindex_grow(m);
        size_t e = m->objs_used++;
        fm_obj *o = &m->objs[e];
        memset(o, 0, sizeof *o);
        o->hash = meta[0];
        o->len = meta[1];
        o->refs = (uint32_t)meta[2];
        o->nchunks = (uint32_t)meta[3];
        m->raw_bytes += o->len * o->refs;
        if (o->nchunks) {
            o->chunk_ix = (uint32_t *)malloc(o->nchunks * sizeof(uint32_t));
            o->cum = (uint64_t *)malloc(o->nchunks * sizeof(uint64_t));
            for (uint32_t c = 0; c < o->nchunks; ++c) {
                uint32_t r;
                if (fread(&r, sizeof r, 1, f) != 1) goto bad;
                o->chunk_ix[c] = cmap[r];
            }
            if (fread(o->cum, sizeof(uint64_t), o->nchunks, f) != o->nchunks)
                goto bad;
        } else {
            o->raw = (unsigned char *)malloc(o->len ? o->len : 1);
            if (fread(o->raw, 1, o->len, f) != o->len) goto bad;
            m->gate_raw_bytes += o->len;
        }
        fm__oindex_insert(m, o->hash, e);
    }
    free(cmap);
    free(tmp);
    fclose(f);
    return 0;
bad:
    free(cmap);
    free(tmp);
    fclose(f);
    return -1;
}

#endif /* FMEM_H */
