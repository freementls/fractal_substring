#!/usr/bin/env bash
# A/B: classic hastok vs tools/fss has — correctness + wall time.
set -euo pipefail
ROOT=/srv/http/fractal_substring
HASTOK=/srv/http/fractal_zip/tools/fractal_compute/hastok
FSS="$ROOT/tools/fss"
[[ -x "$HASTOK" && -x "$FSS" ]] || { echo "missing binary"; exit 2; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

python3 - <<'PY' "$TMP"
import os, struct, sys, time, subprocess
tmpdir = sys.argv[1]
# ~4MB englishish corpus
words = b"the quick brown fox jumps over the lazy dog "
corpus = (words * (4 * 1024 * 1024 // len(words)))[:4 * 1024 * 1024]
path = os.path.join(tmpdir, "corpus.bin")
open(path, "wb").write(corpus)
# ~5k tokens: mix of present fragments and absent
toks = []
for i in range(2500):
    toks.append(words[i % len(words): (i % len(words)) + 4] or b"the ")
for i in range(2500):
    toks.append(f"zz{i:04d}".encode())
req = struct.pack("<I", len(toks))
for t in toks:
    req += struct.pack("<I", len(t)) + t
open(os.path.join(tmpdir, "req.bin"), "wb").write(req)
open(os.path.join(tmpdir, "meta.txt"), "w").write(f"{len(toks)}\n{path}\n")
PY

REQ="$TMP/req.bin"
CORPUS="$TMP/corpus.bin"

run_one() {
  local bin=$1 name=$2
  local t0 t1
  t0=$(date +%s.%N)
  "$bin" "$CORPUS" < "$REQ" > "$TMP/$name.out"
  t1=$(date +%s.%N)
  python3 -c "import os; t0=float('$t0'); t1=float('$t1'); print(f'$name wall_ms={(t1-t0)*1000:.1f} flags={os.path.getsize(\"$TMP/$name.out\")}')"
}

run_one "$HASTOK" hastok
# fss bare = has protocol
run_one "$FSS" fss

python3 - <<PY
import pathlib
a=pathlib.Path("$TMP/hastok.out").read_bytes()
b=pathlib.Path("$TMP/fss.out").read_bytes()
assert a==b, f"mismatch len {len(a)} vs {len(b)} differ={(sum(x!=y for x,y in zip(a,b)))}"
print("KEEP hastok: answers identical")
PY
