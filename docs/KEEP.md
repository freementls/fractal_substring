# KEEP bar results

Measured on this machine (Manjaro, native `-O3 -march=native`).
**Real-corpus numbers** (`php bench/ab_real_sites.php`) are the source of truth.

## Real-site scorecard

| Site | Real input | Speedup |
|------|------------|--------:|
| **COUNT** batch | enwik8 1 MiB + 800 needles | **~40×** (Aho-Corasick) |
| **REPEATS** ASC hybrid | PHP langref ~99 KB | **~300–430×** (trust seeds + skip slide/GPU/multidiff) |
| **REPEATS** immense | 1 MiB periodic tile | **~50 ms/MiB** (skip dense after strong CDC) |
| **HASTOK** `has` | enwik8 4 MiB + 5 k tokens | **~5–9×** (Aho-Corasick) |
| **PEEL** e2e | langref 32 KiB recursive | **~300–900×** (ASC-dominated) |
| **FOLDER peel** | 5× PHP langref (~488 KB) | **~270–300×** (skip multidiff when FSS strong) |
| **PEEL+PACK** | same 5 files → gz | **~170–230×** (ASC-bound; packed size matched) |
| **ZIP** CLI (`SPEED=1`) | same 5-file folder | **~120–200 ms** wall (was ~0.5–2 s); size **44130** (Arc); lifestyle gist skips dominated xz |
| **LOM / find** m=2,3 | enwik8 closers | **~2–30×** vs memmem (memchr dual/triple) |
| **find** m≥4 | enwik8 | dual first+last AVX2; uniform common → memmem |
| **count** m≥2 | enwik8 | dual AVX2 (~3–12× dense shorts / closers) |
| **find_all** | enwik8 | same dual/AVX2 collect (~2–8× vs memmem walk) |

LOM `lomc` construct wall on `perf_fixture.xml` is not dominated by closer scans.

## What landed recently

- **FIND m=2,3:** memchr dual/triple (AVX2 first-byte lost on common `nd[0]`).
- **FIND/COUNT m≥4:** AVX2 **dual first+last** prefilter; common uniform needles (`aaaa`) fall back to memmem.
- **COUNT m≥2:** dual AVX2 (dense `the`/`th` ~3–4× vs memmem walk).
- **find_all:** dual/AVX2 collect (was loop-of-find; now ~count speed).
- **HASTOK:** Aho-Corasick (lazy node init).
- **REPEATS:** trust libfss counts; ≥4 seeds ⇒ skip slide + GPU ASC verify/SA.
- **PEEL / folder peel:** e2e wins from ASC short-circuit.
- **COUNT batch:** Aho-Corasick when k≥48 & n≥4 KiB (~40× enwik); else per-needle count.
- **HASTOK / has_batch:** Aho-Corasick when k≥96 & n≥4 KiB; else sequential find (no O(n) presence tax).
- **REPEATS immense:** gate at 128 KiB (was 192); CDC/fixed then skip dense when strong (~5 ms on 180 KiB bitwise).
- **REPEATS hybrid:** trust libfss seed counts in ASC verify; skip multidiff when seeds strong (≥128 KiB was ~200 ms).
- **REPEATS medium:** chunk≤8 uses stride=`chunk` when scan≥16 KiB (~3× on langref; was stride=1 hash+qsort).
- **ZIP / PEEL+PACK:** folder peel and peel+gz show ASC-bound wins; `SPEED=1` zip wall is often outer-bound.
- **ASC max seed len:** `min(128, max_expr×10)` for `fss_repeats_map` (harvest already ~5 ms; caps long HTML twins).
- **has_batch:** dead multi-length roller removed (AC / sequential find only).
- **find_all PHP:** `fss_ext_find_all` + ASC reciprocal skip intervals (multidiff/FSS-weak path).
- **pair_byte_diff:** counts via `fractal_zip_fss_substr_count`.
- **ZIP SPEED/lifestyle:** all-text mid hybrids include `.php`/HTML; skip xz tip when Arc already ≤¼ raw (`midHybridArcStrong`) — same **44130** Arc wire, ~**4–10×** wall vs prior xz double-probe.
- **ZIP SPEED strong Arc:** under `FRACTAL_ZIP_SPEED=1`, skip zpaq/7z/native race when multifile Arc ≤10% raw (php langref 57: ~40 s→~2 s at Arc wire; lifestyle without SPEED keeps zpaq bytes).
- **ZIP SPEED early zpaq gates:** all-.zip FZB/pack zpaq (~37 s→~1–2 s zstd on 65); all-image zpaq / tar|br11 (~5–10 s→~0.5 s Arc-m5 on 168); BMP post-Arc zpaq skipped under SPEED. Lifestyle (no SPEED) keeps zpaq byte tips.
- **ZIP SPEED large singles:** skip CM-first / paq8px / binary-cm / late zpaq-noattr; midsize singles use Arc -m1 (not mx=9 7z / m5); skip weak-Arc xz tip under SPEED. Lifestyle/ultra keep CM/paq/7z tips.
- **ZIP SPEED dense/media forests:** Arc -m1 for ≥64-member / media-heavy packs (langref 57 ~2.5 s→~1.1 s; 80/61 sub-second); binary mid-hybrid Arc-m1 instead of xz (194 ~1.8 s→~0.06 s).

## Cliff note

Peel ≈40 ms. ZIP `SPEED=1` on the 5-file corpus is **lifestyle native Arc** (not unified ASC): ~**135 ms** median at **44130** bytes after skipping dominated xz probes on strong Arc / all-text mid hybrids (`.php`/HTML prose). Ultra (lifestyle off) still lands **43982** zpaq on the same corpus — separate Pareto.

## Re-run

```bash
export TMPDIR=/srv/http/fractal_substring/.tmp
make && make php-ext && make test
php bench/ab_real_sites.php
php bench/ab_folder_peel.php
php bench/ab_peel_pack.php
bash bench/ab_zip_folder.sh
```
