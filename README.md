# fractal_substring — vision substring engine (libfss)

Fast exact substring search for tools that need it (`fractal_zip`, `LOM`, …).
Gist-then-foveate: cheap profile of the whole string, then SIMD/Horspool only
where needed. Optional `fmem` / `fcache` for interned haystacks and memoization.

**License:** [Apache License 2.0](LICENSE).

Status: [docs/STATUS.md](docs/STATUS.md) (v1 KEEP) · KEEP: [docs/KEEP.md](docs/KEEP.md) · [CHANGELOG](docs/CHANGELOG.md) · [INTEGRATION](docs/INTEGRATION.md)

## Build

```bash
cd /srv/http/fractal_substring   # or your checkout
make
make test
make php-ext   # bindings/php_ext/modules/fss.so
make bench
```

Install (optional): copy `libfss.so` onto `LD_LIBRARY_PATH` / `/usr/local/lib` so LOM can `dlopen` it without a hardcoded path.

## Integration (see docs/KEEP.md / docs/INTEGRATION.md)

| Site | Default | Notes |
|------|---------|-------|
| HASTOK | **ON** | `tools/fss` ~5–9× vs classic hastok (enwik) |
| COUNT | **ON** | needs `fss.so`; ~40× batch vs `substr_count` |
| REPEATS | **ON** | ASC hybrid ~100×+ (skip slide + GPU re-verify) |
| PEEL / folder | **ON** | recursive peel ~15–280× when ASC-bound |
| LOM scan | **ON** | `libfss.so` via dlopen; ~2–30× short closers |

Consumers resolve the library via env or a sibling checkout:

- `FRACTAL_ZIP_FSS_ROOT` / `FRACTAL_ZIP_FSS_EXT` / `FRACTAL_ZIP_FSS_LIB` / `FRACTAL_ZIP_FSS_BIN`
- `LOM_FSS_LIB` (else `libfss.so` on the linker path, then common install locations, then `<parent>/fractal_substring/libfss.so` next to `LOM/`)

```bash
export FRACTAL_ZIP_FSS=0            # hard off
export FRACTAL_ZIP_FSS_HASTOK=0     # classic hastok
export FRACTAL_ZIP_FSS_COUNT=0
export FRACTAL_ZIP_FSS_REPEATS=0
export LOM_FSS_SCAN=0
```
