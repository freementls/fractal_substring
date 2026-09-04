#!/usr/bin/env php
<?php
/**
 * Peel-then-pack A/B: recursive_fractal_substring dominates; pack is cheap.
 * Shows the zip-relevant win when ASC is on the critical path (unlike SPEED=1
 * folder zip, which is often outer/literal-bound).
 */
declare(strict_types=1);

$root = dirname(__DIR__);
$ext = $root . '/bindings/php_ext/modules/fss.so';
$corp = $root . '/.tmp/peel_ab_corp';

putenv('TMPDIR=' . $root . '/.tmp');
putenv('FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC=1');
putenv('FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1');
putenv('FRACTAL_ZIP_NO_CLI_FFI_REEXEC=1');

if (is_file($ext) && !extension_loaded('fss') && !in_array('--with-fss', $argv, true)) {
	$php = PHP_BINARY !== '' ? PHP_BINARY : 'php';
	passthru(escapeshellarg($php) . ' -d extension=' . escapeshellarg($ext)
		. ' ' . escapeshellarg(__FILE__) . ' --with-fss', $code);
	exit($code);
}

if (!is_dir($corp)) {
	fwrite(STDERR, "missing $corp — run bench/ab_folder_peel.php first or copy fixtures\n");
	exit(2);
}

require_once '/srv/http/fractal_zip/fractal_zip.php';

$files = array();
foreach (scandir($corp) as $name) {
	if ($name === '.' || $name === '..') {
		continue;
	}
	$path = $corp . '/' . $name;
	if (is_file($path)) {
		$files[] = $path;
	}
}

$run = static function (array $files, int $fss) use ($corp): array {
	putenv('FRACTAL_ZIP_FSS=' . $fss);
	putenv('FRACTAL_ZIP_FSS_HASTOK=' . $fss);
	putenv('FRACTAL_ZIP_FSS_COUNT=' . $fss);
	putenv('FRACTAL_ZIP_FSS_REPEATS=' . $fss);
	$peelMs = 0.0;
	$packMs = 0.0;
	$rawTotal = 0;
	$packed = '';
	foreach ($files as $path) {
		$raw = file_get_contents($path);
		if ($raw === false) {
			continue;
		}
		$rawTotal += strlen($raw);
		$fz = new fractal_zip(256, false, false, null, false);
		$t0 = hrtime(true);
		$out = $fz->recursive_fractal_substring($raw, $raw);
		$peelMs += (hrtime(true) - $t0) / 1e6;
		$blob = is_string($out) ? $out : $raw;
		$t0 = hrtime(true);
		$gz = gzencode($blob, 6);
		$packMs += (hrtime(true) - $t0) / 1e6;
		$packed .= is_string($gz) ? $gz : $blob;
	}
	return array($peelMs, $packMs, $rawTotal, strlen($packed));
};

[$p0, $k0, $bytes, $z0] = $run($files, 0);
[$p1, $k1, $_, $z1] = $run($files, 1);
$wall0 = $p0 + $k0;
$wall1 = $p1 + $k1;

printf(
	"PEEL+PACK files=%d raw=%d peel0=%.0fms pack0=%.0fms wall0=%.0fms peel1=%.0fms pack1=%.0fms wall1=%.0fms speedup=%.2fx packed0=%d packed1=%d\n",
	count($files),
	$bytes,
	$p0,
	$k0,
	$wall0,
	$p1,
	$k1,
	$wall1,
	$wall0 / max($wall1, 0.001),
	$z0,
	$z1
);
$keep = ($wall0 / max($wall1, 0.001)) >= 1.5;
echo $keep ? "KEEP peel+pack: PASS\n" : "KEEP peel+pack: FAIL\n";
exit($keep ? 0 : 1);
