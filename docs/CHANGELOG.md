# Changelog

## v1.0 — vision substring engine (KEEP)

Shipped and gated into fractal_zip / LOM when beneficial.

### Library
- Exact find / count / find_all: memchr dual/triple (m=2,3 find); AVX2 dual first+last (m≥4 find, m≥2 count)
- `has_batch` Aho-Corasick (k≥96, n≥4 KiB); else sequential find
- `count_batch` Aho-Corasick (k≥48, n≥4 KiB); else per-needle count
- `repeats` CDC + fixed twins + verify budget (ASC seed source)
- Optional fmem/fcache store API

### Integration (default ON when available)
- **HASTOK** → `tools/fss` (~5–9× enwik)
- **COUNT** → `fss.so` (~40× batch via Aho-Corasick)
- **REPEATS** → hybrid ASC: ≥4 seeds skips slide + GPU re-verify (~100×+); ASC `max_len` capped at 128
- **PEEL / folder / peel+pack** → ASC-bound e2e (~270–900× folder/32 KiB)
- **ZIP** (`SPEED=1`) → lifestyle Arc passthrough ~110–200 ms at 44130 B on KEEP; large-single / dense-forest / mid-hybrid SPEED seeds use Arc-m1 (skip CM/paq/zpaq/mx9/xz tips)
- **LOM scan** → `lom_fss_memmem` (~2–30× short closers via memchr dual/triple)

### Housekeeping
- Removed unused multi-length rolling `has_batch` path (find-loop / AC only)
- Rare-gram harvest strides earlier on mid-size scans
- Bridge helper `fractal_zip_fss_strpos` (offset=0 → `fss_php_find`; not faster than PHP strpos on typical needles)
- PHP `fss_ext_find_all` / `fss_php_find_all` + ASC reciprocal skip-interval wiring
- ASC `pair_byte_diff_seeds` uses `fractal_zip_fss_substr_count` (FSS-weak / multidiff fallback)

### Verify
```bash
export TMPDIR=/srv/http/fractal_substring/.tmp
make && make smoke
php bench/ab_real_sites.php
```

Hard off: `FRACTAL_ZIP_FSS=0` / `LOM_FSS_SCAN=0`.
