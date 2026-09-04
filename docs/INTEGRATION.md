# Integration status

## Wired

| Project | Site | Default | Disable |
|---------|------|---------|---------|
| fractal_zip | hastok batch | **ON** (`tools/fss`) | `FRACTAL_ZIP_FSS_HASTOK=0` |
| fractal_zip | count verify | **ON** (PHP `fss.so`) | `FRACTAL_ZIP_FSS_COUNT=0` |
| fractal_zip | repeats | **ON** (`fss.so` or `tools/fss`, hybrid) | `FRACTAL_ZIP_FSS_REPEATS=0` |
| LOM native | scan / fstr / doc probes | **ON** (`libfss.so`, n≥4 KiB; memchr dual/triple short find) | `LOM_FSS_SCAN=0` |

Hard off: `FRACTAL_ZIP_FSS=0` / `LOM_FSS=0`.

Path discovery (no hard dependency on `/srv/http/…`):

- **fractal_zip:** `FRACTAL_ZIP_FSS_ROOT`, else sibling `../fractal_substring/`, else `/srv/http/fractal_substring/`
- **LOM:** `LOM_FSS_LIB`, else `libfss.so` / `/usr/local/lib`, else sibling of the `LOM/` tree, else `/srv/http/fractal_substring/libfss.so`

CLI auto-loads the PHP extension via re-exec in
`fractal_zip_cli_opcache_bootstrap.php` (same pattern as FFI). Opt out:
`FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1`. Note the bootstrap spellings
`FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC` / `FRACTAL_ZIP_INTERNAL_CLI_OPACHE`.

With ≥4 libfss repeat seeds, ASC skips the legacy slide **and** GPU
verify/SA merge (counts already verified in C).

See [KEEP.md](KEEP.md) for measured bars.

## Census — not wired

Re-run: `bench/census_callers.sh`. Deliberately left alone:

| Area | Why |
|------|-----|
| fractal_zip PDF pac / stream | Small buffers; KEEP would not pay |
| LOM `O.php` selector `strpos` | Tiny strings / selector noise |
| fis / diff | Not production peel paths |
| LOM `lom_doc` short tag `strstr` | Short tags; plan excludes |

Hot paths already gated: hastok, ASC count verify, ASC repeats hybrid, LOM scan.

## Verify

```bash
make test && make php-ext
./bench/ab_hastok.sh
php bench/ab_repeats_hybrid.php
php bench/ab_folder_peel.php
bash bench/ab_zip_folder.sh
php -d extension=bindings/php_ext/modules/fss.so tests/verify_integrate.php
php tests/smoke_peel_fss.php
```
