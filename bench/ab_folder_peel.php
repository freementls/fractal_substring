#!/usr/bin/env php
<?php
/**
 * Multi-file peel A/B: sum recursive_fractal_substring over a small folder.
 */
declare(strict_types=1);

$root = dirname(__DIR__);
$ext = $root . '/bindings/php_ext/modules/fss.so';
$corp = $root . '/.tmp/peel_ab_corp';
foreach (array_slice($argv, 1) as $arg) {
	if ($arg === '--with-fss') {
		continue;
	}
	$corp = $arg;
	break;
}

putenv('TMPDIR=' . $root . '/.tmp');
putenv('FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC=1');
putenv('FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1');
putenv('FRACTAL_ZIP_NO_CLI_FFI_REEXEC=1');

if (is_file($ext) && !extension_loaded('fss') && !in_array('--with-fss', $argv, true)) {
	$php = PHP_BINARY !== '' ? PHP_BINARY : 'php';
	passthru(escapeshellarg($php) . ' -d extension=' . escapeshellarg($ext)
		. ' ' . escapeshellarg(__FILE__) . ' --with-fss ' . escapeshellarg($corp), $code);
	exit($code);
}

if (!is_dir($corp)) {
	fwrite(STDERR, "missing corpus dir: $corp\n");
	exit(2);
}

require_once '/srv/http/fractal_zip/fractal_zip.php';

$files = array();
foreach (scandir($corp) as $name) {
	if ($name === '.' || $name === '..') {
		continue;
	}
	$path = $corp . '/' . $name;
	if (is_file($path) && is_readable($path)) {
		$files[] = $path;
	}
}
if ($files === array()) {
	fwrite(STDERR, "no files in $corp\n");
	exit(2);
}

$peelAll = static function (array $files): array {
	$totalMs = 0.0;
	$bytesIn = 0;
	foreach ($files as $path) {
		$raw = file_get_contents($path);
		if ($raw === false) {
			continue;
		}
		$bytesIn += strlen($raw);
		$fz = new fractal_zip(256, false, false, null, false);
		$t0 = hrtime(true);
		$fz->recursive_fractal_substring($raw, $raw);
		$totalMs += (hrtime(true) - $t0) / 1e6;
	}
	return array($totalMs, $bytesIn);
};

putenv('FRACTAL_ZIP_FSS=0');
[$offMs, $bytes] = $peelAll($files);
putenv('FRACTAL_ZIP_FSS=1');
putenv('FRACTAL_ZIP_FSS_HASTOK=1');
putenv('FRACTAL_ZIP_FSS_COUNT=1');
putenv('FRACTAL_ZIP_FSS_REPEATS=1');
[$onMs, $_] = $peelAll($files);

printf("FOLDER peel files=%d bytes=%d FSS=0:%.0fms FSS=1:%.0fms speedup=%.2fx\n",
	count($files), $bytes, $offMs, $onMs, $offMs / max($onMs, 0.001));
$keep = ($offMs / max($onMs, 0.001)) >= 1.5;
echo $keep ? "KEEP folder peel: PASS\n" : "KEEP folder peel: FAIL\n";
exit($keep ? 0 : 1);
