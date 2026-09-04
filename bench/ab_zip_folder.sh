#!/usr/bin/env bash
# A/B: fractal_zip_cli zip — SPEED Pareto KEEP + wall report.
# SPEED=1 + lifestyle: time at fixed SPEED champion bytes (not ASC-bound).
# Peel benches remain the ASC e2e proof.
set -euo pipefail
ROOT=/srv/http/fractal_substring
CLI=/srv/http/fractal_zip/fractal_zip_cli.php
TMPDIR="${TMPDIR:-$ROOT/.tmp}"
export TMPDIR
mkdir -p "$TMPDIR"

# SPEED champion on the 5-file PHP langref KEEP corpus (bytes never worse).
SPEED_ZIP_CHAMPION_BYTES="${SPEED_ZIP_CHAMPION_BYTES:-44130}"
# Direct zip_folder wall (excludes CLI cold-compile). Pre-cut ~500–900ms; post-cut ~120–200ms.
SPEED_ZIP_FOLDER_MAX_MS="${SPEED_ZIP_FOLDER_MAX_MS:-400}"

CORP="${1:-$TMPDIR/peel_ab_corp}"
if [[ ! -d "$CORP" ]]; then
  mkdir -p "$CORP"
  i=0
  for f in /srv/http/fractal_zip/test_files57/phplangref-try2/language.operators.*.php; do
    [[ -f "$f" ]] || continue
    cp "$f" "$CORP/"
    i=$((i + 1))
    [[ $i -ge 5 ]] && break
  done
fi

EXT0="$TMPDIR/zip_ab_ext0"
EXT1="$TMPDIR/zip_ab_ext1"
rm -rf "$EXT0" "$EXT1"
mkdir -p "$EXT0" "$EXT1"

run_one() {
  local fss=$1 outdir=$2
  rm -f "${CORP}.fz"
  local t0 t1
  t0=$(date +%s.%N)
  env FRACTAL_ZIP_SPEED=1 \
      FRACTAL_ZIP_CLI_VERBOSE=0 \
      FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC=1 \
      FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1 \
      FRACTAL_ZIP_NO_CLI_FFI_REEXEC=1 \
      FRACTAL_ZIP_FSS="$fss" \
      FRACTAL_ZIP_FSS_HASTOK="$fss" \
      FRACTAL_ZIP_FSS_COUNT="$fss" \
      FRACTAL_ZIP_FSS_REPEATS="$fss" \
      php "$CLI" zip "$CORP" >/dev/null
  t1=$(date +%s.%N)
  cp -f "${CORP}.fz" "$outdir/c.fz"
  php "$CLI" extract "$outdir/c.fz" >/dev/null
  python3 -c "print(int(($t1-$t0)*1000))"
}

# Direct zip_folder median (3 runs) — SPEED compressor Pareto, not CLI bootstrap.
folder_med_ms=$(php -d opcache.enable_cli=0 -r '
putenv("TMPDIR=" . getenv("TMPDIR"));
putenv("FRACTAL_ZIP_SPEED=1");
putenv("FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC=1");
putenv("FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1");
putenv("FRACTAL_ZIP_NO_CLI_FFI_REEXEC=1");
putenv("FRACTAL_ZIP_LIFESTYLE_SPEED_PROFILE=1");
require "/srv/http/fractal_zip/fractal_zip.php";
$dir = $argv[1];
$times = array();
$sz = -1;
$codec = "";
for ($i = 0; $i < 3; $i++) {
	@unlink($dir . ".fz");
	$fz = new fractal_zip(null, true, true, null, true);
	$t = hrtime(true);
	$fz->zip_folder($dir, false);
	$times[] = (hrtime(true) - $t) / 1e6;
	$sz = filesize($dir . ".fz");
	$codec = (string) fractal_zip::$folder_zip_wire_best_outer_codec;
}
sort($times);
echo (int) round($times[1]) . " " . (int) $sz . " " . $codec;
' "$CORP")
folder_ms=$(echo "$folder_med_ms" | awk '{print $1}')
folder_sz=$(echo "$folder_med_ms" | awk '{print $2}')
folder_codec=$(echo "$folder_med_ms" | awk '{print $3}')

off_ms=$(run_one 0 "$EXT0")
on_ms=$(run_one 1 "$EXT1")
off_sz=$(stat -c%s "$EXT0/c.fz")
on_sz=$(stat -c%s "$EXT1/c.fz")
speed=$(python3 -c "print(f'{$off_ms/max($on_ms,1):.2f}')")

echo "ZIP folder bytes_in=$(du -sb "$CORP" | awk '{print $1}') FSS=0:${off_ms}ms FSS=1:${on_ms}ms speedup=${speed}x fz_off=${off_sz} fz_on=${on_sz} zip_folder_med=${folder_ms}ms codec=${folder_codec} sz=${folder_sz}"

python3 <<PY
import os, sys
corp = "$CORP"
champ = int("$SPEED_ZIP_CHAMPION_BYTES")
max_folder_ms = int("$SPEED_ZIP_FOLDER_MAX_MS")
off, on = int("$off_ms"), int("$on_ms")
off_sz, on_sz = int("$off_sz"), int("$on_sz")
folder_ms, folder_sz = int("$folder_ms"), int("$folder_sz")

for tag, root in (("0", "$EXT0"), ("1", "$EXT1")):
    n = 0
    for name in os.listdir(corp):
        src = os.path.join(corp, name)
        if not os.path.isfile(src):
            continue
        got = None
        for dp, _, fs in os.walk(root):
            if name in fs:
                got = os.path.join(dp, name)
                break
        if got is None:
            print(f"KEEP zip folder: FAIL (missing extract {name} fss={tag})", file=sys.stderr)
            sys.exit(1)
        if open(src, "rb").read() != open(got, "rb").read():
            print(f"KEEP zip folder: FAIL (roundtrip mismatch {name} fss={tag})", file=sys.stderr)
            sys.exit(1)
        n += 1
    if n < 1:
        print("KEEP zip folder: FAIL (no members)", file=sys.stderr)
        sys.exit(1)

for tag, sz in (("0", off_sz), ("1", on_sz), ("folder", folder_sz)):
    if sz > champ:
        print(f"KEEP zip folder: FAIL ({tag}_sz={sz} > champion {champ})", file=sys.stderr)
        sys.exit(1)
if folder_ms > max_folder_ms:
    print(f"KEEP zip folder: FAIL (zip_folder_med={folder_ms}ms > max {max_folder_ms}ms)", file=sys.stderr)
    sys.exit(1)

print("KEEP zip folder: PASS (roundtrip ok, size≤champion, zip_folder wall≤max)")
PY
