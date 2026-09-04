#!/usr/bin/env php
<?php
/**
 * A/B hybrid: ASC with FSS_REPEATS merge+skip-slide vs legacy ASC.
 */
declare(strict_types=1);

$zip = '/srv/http/fractal_zip';
$path = $argv[1] ?? $zip . '/test_files57/phplangref-try2/language.operators.arithmetic.php';
$max = isset($argv[2]) ? (int) $argv[2] : 32768;
$raw = file_get_contents($path);
if ($raw === false) {
	exit(2);
}
if (strlen($raw) > $max) {
	$raw = substr($raw, 0, $max);
}
echo 'bytes=' . strlen($raw) . "\n";

putenv('FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC=1');
putenv('FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1');
/* Isolate REPEATS: COUNT speeds the legacy slide more than hybrid. */
putenv('FRACTAL_ZIP_FSS_COUNT=0');
require_once $zip . '/fractal_zip.php';

putenv('FRACTAL_ZIP_FSS_REPEATS=0');
$fz = new fractal_zip(256, false, false, null, false);
$t0 = hrtime(true);
$a = $fz->all_substrings_count($raw);
$t1 = hrtime(true);

putenv('FRACTAL_ZIP_FSS_REPEATS=1');
/* Re-require site check — getenv is live; site_enabled reads env each call */
$fz2 = new fractal_zip(256, false, false, null, false);
$t2 = hrtime(true);
$b = $fz2->all_substrings_count($raw);
$t3 = hrtime(true);

$legacyMs = ($t1 - $t0) / 1e6;
$fssMs = ($t3 - $t2) / 1e6;
$speedup = $fssMs > 0 ? $legacyMs / $fssMs : 0;

/* Shared keys */
$shared = 0;
foreach ($b as $k => $_) {
	if (isset($a[$k])) {
		$shared++;
	}
}
$overlap = count($b) > 0 ? $shared / count($b) : 0;

$slen = strlen($raw);
$marker = 4;
$bestEst = static function (array $map) use ($slen, $marker): int {
	$best = PHP_INT_MAX;
	foreach ($map as $sub => $c) {
		$len = strlen((string) $sub);
		$c = (int) $c;
		if ($len < 2 || $c < 2) {
			continue;
		}
		$est = $len + $slen - $c * ($len - $marker);
		if ($est < $best) {
			$best = $est;
		}
	}
	return $best === PHP_INT_MAX ? 0 : $best;
};
$bestA = $bestEst($a);
$bestB = $bestEst($b);

echo sprintf("legacy=%.1fms n=%d best_estLin=%d\n", $legacyMs, count($a), $bestA);
echo sprintf("fss_hybrid=%.1fms n=%d best_estLin=%d\n", $fssMs, count($b), $bestB);
echo sprintf("overlap=%.0f%% speedup=%.2fx estLin_delta=%d\n",
	$overlap * 100, $speedup, $bestB - $bestA);
/* Wall KEEP; exact key overlap is not required (shape differs). Prefer non-worse best estLin. */
$keep = ($speedup >= 1.5 && count($b) >= 2 && ($bestB === 0 || $bestA === 0 || $bestB <= $bestA + 256));
echo $keep ? "KEEP repeats hybrid: PASS\n" : "KEEP repeats hybrid: FAIL\n";
exit($keep ? 0 : 1);
