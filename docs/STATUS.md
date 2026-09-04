# fractal_substring status

Vision substring engine (`libfss`) — **v1 KEEP complete**.

Gated accelerator for fractal_zip / LOM. See [CHANGELOG.md](CHANGELOG.md).

## Sites (default ON when available)

| Site | Gate | Real win |
|------|------|----------|
| HASTOK | `tools/fss` | **~5–9×** enwik batch (Aho-Corasick) |
| COUNT | `fss.so` | **~40×** multi-pattern batch (Aho-Corasick) |
| REPEATS | `fss.so` / CLI | **~300–400×** ASC hybrid |
| PEEL e2e | all sites | **~300–900×** (32 KiB) / **~270–300×** (5-file folder) |
| PEEL+PACK | folder → gz | **~170–300×** (ASC-bound; packed size matched) |
| ZIP CLI | `SPEED=1` folder | **~120–200 ms**, size **44130** (lifestyle Arc; not ASC); hot forests skip zpaq under SPEED |
| LOM scan | `libfss.so` n≥4 KiB | **~2–30×** short closers (memchr dual/triple) |

Hard off: `FRACTAL_ZIP_FSS=0` / `LOM_FSS_SCAN=0`.

## Verify

```bash
export TMPDIR=/srv/http/fractal_substring/.tmp
make && make smoke
php bench/ab_real_sites.php
php bench/ab_folder_peel.php
php bench/ab_peel_pack.php
bash bench/ab_zip_folder.sh
```

See [KEEP.md](KEEP.md).
