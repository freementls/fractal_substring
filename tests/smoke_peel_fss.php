#!/usr/bin/env php
<?php
/**
 * Smoke: fractal_zip peel path sees libfss (REPEATS hybrid + COUNT).
 * REPEATS speedup is measured with COUNT off so the slide-skip win is isolated
 * (COUNT accelerates legacy slide more than hybrid).
 */
declare(strict_types=1);

$fssRoot = dirname(__DIR__);
require_once $fssRoot . '/bindings/fss.php';

$fzPath = '/srv/http/fractal_zip/fractal_zip.php';
if (!is_file($fzPath)) {
	fwrite(STDERR, "skip: fractal_zip missing\n");
	exit(0);
}
require_once $fzPath;

$fail = 0;
if (!fss_php_site_enabled('REPEATS')) {
	fwrite(STDERR, "FAIL: REPEATS site not enabled\n");
	$fail++;
}
$viaExt = fss_php_ensure_ext() && function_exists('fss_ext_repeats');
$viaCli = fss_php_bin_path() !== null;
if (!$viaExt && !$viaCli) {
	fwrite(STDERR, "FAIL: neither fss.so repeats nor tools/fss\n");
	$fail++;
}
echo $viaExt ? "repeats_via=ext\n" : "repeats_via=cli\n";

$fixture = '/srv/http/fractal_zip/test_files57/phplangref-try2/language.operators.arithmetic.php';
if (!is_file($fixture)) {
	fwrite(STDERR, "skip: fixture missing\n");
	exit($fail ? 1 : 0);
}
$hay = file_get_contents($fixture);
if (strlen($hay) > 32768) {
	$hay = substr($hay, 0, 32768);
}

putenv('FRACTAL_ZIP_FSS_COUNT=0');
putenv('FRACTAL_ZIP_FSS_REPEATS=1');
$map = fractal_zip_fss_repeats_map($hay, 4, 256, 24);
if (!is_array($map) || count($map) < 4) {
	fwrite(STDERR, "FAIL: expected ≥4 fss repeat seeds\n");
	$fail++;
} else {
	echo 'repeats_seeds=' . count($map) . "\n";
}

$fzOff = new fractal_zip(256, false, false, null, false);
putenv('FRACTAL_ZIP_FSS_REPEATS=0');
$t0 = hrtime(true);
$off = $fzOff->all_substrings_count($hay);
$msOff = (hrtime(true) - $t0) / 1e6;

$fzOn = new fractal_zip(256, false, false, null, false);
putenv('FRACTAL_ZIP_FSS_REPEATS=1');
$t0 = hrtime(true);
$on = $fzOn->all_substrings_count($hay);
$msOn = (hrtime(true) - $t0) / 1e6;

if (!is_array($off) || !is_array($on) || $on === array()) {
	fwrite(STDERR, "FAIL: all_substrings_count empty\n");
	$fail++;
} else {
	$speed = $msOn > 0 ? $msOff / $msOn : 0;
	echo sprintf("asc_off=%.0fms n=%d  asc_on=%.0fms n=%d  speedup=%.2fx\n",
		$msOff, count($off), $msOn, count($on), $speed);
	if ($speed < 1.3) {
		fwrite(STDERR, "FAIL: hybrid not faster enough (need ≥1.3×)\n");
		$fail++;
	}
}

/* COUNT correctness (re-enable). */
putenv('FRACTAL_ZIP_FSS_COUNT=1');
$probe = 'function';
$cnt = fractal_zip_fss_substr_count($hay, $probe);
$want = substr_count($hay, $probe);
if ($cnt !== $want) {
	fwrite(STDERR, "FAIL: count want=$want got=$cnt\n");
	$fail++;
}

if ($fail) {
	fwrite(STDERR, "$fail failure(s)\n");
	exit(1);
}
echo "smoke_peel_fss ok\n";
