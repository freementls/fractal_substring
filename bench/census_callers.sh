#!/usr/bin/env bash
# Census /srv/http for substring hot paths that might benefit from libfss.
# Reports candidates; does not modify anything.
set -euo pipefail
ROOT=/srv/http
echo "=== fss integration census ==="
echo "Looking for hot-path memmem/strpos/substr_count/str_contains under $ROOT"
echo

for proj in fractal_zip LOM fractal diff fis; do
  d="$ROOT/$proj"
  [[ -d "$d" ]] || continue
  echo "## $proj"
  # Count likely hot calls (heuristic)
  rg -l --glob '!**/vendor/**' --glob '!**/node_modules/**' --glob '!**/.git/**' \
    -e 'memmem\(' -e 'substr_count\(' -e 'str_contains\(' -e 'strpos\(\$' \
    "$d" 2>/dev/null | head -30 || true
  echo
done

echo "## Already wired"
echo "  fractal_substring: libfss + tools/fss + bindings/fss.php"
echo "  fractal_zip: fractal_zip_fss.php (COUNT/HASTOK/REPEATS — default ON when available)"
echo "  LOM native: lom_fss.h via lom_fss_memmem (default ON for n≥4KiB; LOM_FSS_SCAN=0 off)"
echo
echo "Skip: fis (editor), process guards, path checks, tiny selector strpos."
