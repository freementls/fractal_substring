#!/usr/bin/env bash
# Ensure .tmp/peel_ab_corp exists for folder/peel+pack/zip benches.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORP="${1:-$ROOT/.tmp/peel_ab_corp}"
mkdir -p "$CORP"
shopt -s nullglob
files=(/srv/http/fractal_zip/test_files57/phplangref-try2/language.operators.*.php)
if ((${#files[@]} == 0)); then
  echo "ensure_peel_corpus: no langref fixtures" >&2
  exit 1
fi
i=0
for f in "${files[@]}"; do
  cp -f "$f" "$CORP/"
  i=$((i + 1))
  ((i >= 5)) && break
done
echo "peel_ab_corp files=$i dir=$CORP"
