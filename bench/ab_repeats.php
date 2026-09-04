#!/usr/bin/env php
<?php
/**
 * A/B: fss repeats vs legacy all_substrings_count.
 *
 * Usage: php bench/ab_repeats.php [path] [max_bytes=65536]
 */
declare(strict_types=1);

$root = dirname(__DIR__);
$zip = '/srv/http/fractal_zip';
$path = $argv[1] ?? $zip . '/test_files57/phplangref-try2/language.operators.arithmetic.php';
$max = isset($argv[2]) ? (int) $argv[2] : 65536;

if (!is_file($path)) {
	fwrite(STDERR, "missing $path\n");
	exit(2);
}
$raw = file_get_contents($path);
if ($raw === false) {
	exit(2);
}
if (strlen($raw) > $max) {
	$raw = substr($raw, 0, $max);
}
$n = strlen($raw);
echo "corpus=$path bytes=$n\n";

/* ---- fss repeats CLI ---- */
$bin = $root . '/tools/fss';
$tmp = tempnam(sys_get_temp_dir(), 'fssr');
file_put_contents($tmp, $raw);
$minL = 4;
$maxL = 128;
$topK = 16;
$t0 = hrtime(true);
$out = [];
$code = 0;
exec(escapeshellarg($bin) . ' repeats ' . escapeshellarg($tmp)
	. " $minL $maxL $topK 2>/dev/null", $out, $code);
$t1 = hrtime(true);
@unlink($tmp);
$fssMap = [];
foreach ($out as $line) {
	$parts = explode("\t", $line, 4);
	if (count($parts) < 4) {
		continue;
	}
	$fssMap[$parts[3]] = (int) $parts[1];
}
$fssMs = ($t1 - $t0) / 1e6;
echo sprintf("fss_repeats: %.1fms candidates=%d code=%d\n", $fssMs, count($fssMap), $code);

/* ---- legacy ASC (force REPEATS off) ---- */
putenv('FRACTAL_ZIP_FSS_REPEATS=0');
putenv('FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC=1'); /* keep this process for timing */
putenv('FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1');
require_once $zip . '/fractal_zip.php';
$fz = new fractal_zip(256, false, false, null, false);
$t2 = hrtime(true);
$asc = $fz->all_substrings_count($raw);
$t3 = hrtime(true);
$ascMs = ($t3 - $t2) / 1e6;
echo sprintf("legacy_ASC: %.1fms candidates=%d\n", $ascMs, count($asc));

/* Overlap of keys (fss needles may be truncated in CLI print to 64 bytes) */
$ascKeys = array_keys($asc);
$hit = 0;
foreach ($fssMap as $sub => $cnt) {
	if (isset($asc[$sub])) {
		$hit++;
		continue;
	}
	/* prefix match if CLI truncated */
	foreach ($ascKeys as $ak) {
		if (str_starts_with($ak, $sub) || str_starts_with($sub, $ak)) {
			$hit++;
			break;
		}
	}
}
$overlap = count($fssMap) > 0 ? $hit / count($fssMap) : 0.0;
$speedup = $ascMs > 0 ? $fssMs > 0 ? $ascMs / $fssMs : INF : 0;
echo sprintf("overlap=%.0f%% speedup=%.2fx (KEEP: >=1.5x wall, useful overlap)\n",
	$overlap * 100, $speedup);

$keep = ($speedup >= 1.5 && $overlap >= 0.25 && count($fssMap) >= 2);
echo $keep ? "KEEP repeats: PASS\n" : "KEEP repeats: FAIL (full-replace unsafe; hybrid is default)\n";
exit($keep ? 0 : 1);
