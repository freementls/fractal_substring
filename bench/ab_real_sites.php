#!/usr/bin/env php
<?php
/**
 * Real-corpus scorecard for all fss sites (honest A/B).
 */
declare(strict_types=1);

$root = dirname(__DIR__);
$ext = $root . '/bindings/php_ext/modules/fss.so';
$fz = '/srv/http/fractal_zip';
$enwik = $fz . '/test_files109/enwik8';
$langref = $fz . '/test_files57/phplangref-try2/language.operators.arithmetic.php';

putenv('TMPDIR=' . $root . '/.tmp');
@mkdir($root . '/.tmp', 0775, true);
/* Prevent fractal_zip CLI bootstrap from re-execing away our -d extension=. */
putenv('FRACTAL_ZIP_NO_CLI_OPACHE_REEXEC=1');
putenv('FRACTAL_ZIP_NO_CLI_FSS_REEXEC=1');
putenv('FRACTAL_ZIP_NO_CLI_FFI_REEXEC=1');

$already = in_array('--with-fss', $argv, true);
if (is_file($ext) && !extension_loaded('fss') && !$already) {
	$php = PHP_BINARY !== '' ? PHP_BINARY : 'php';
	$cmd = escapeshellarg($php) . ' -d extension=' . escapeshellarg($ext)
		. ' ' . escapeshellarg(__FILE__) . ' --with-fss';
	passthru($cmd, $code);
	exit($code);
}

echo "=== fss real-site scorecard ===\n";
echo 'fss.so=' . (extension_loaded('fss') ? 'yes' : 'no') . "\n";

/* COUNT */
$h = file_get_contents($enwik, false, null, 0, 1 << 20);
$needles = array();
for ($i = 0; $i < 800; $i++) {
	$o = ($i * 7919) % (strlen($h) - 16);
	$needles[] = substr($h, $o, 3 + ($i % 12));
}
$t0 = hrtime(true);
foreach ($needles as $x) {
	substr_count($h, $x);
}
$aMs = (hrtime(true) - $t0) / 1e6;
$t0 = hrtime(true);
if (function_exists('fss_ext_count_batch')) {
	fss_ext_count_batch($h, $needles);
} else {
	foreach ($needles as $x) {
		substr_count($h, $x);
	}
}
$bMs = (hrtime(true) - $t0) / 1e6;
printf("COUNT enwik1MiB+800 php=%.1fms fss=%.1fms speedup=%.2fx\n",
	$aMs, $bMs, $aMs / max($bMs, 0.001));

/* ASC / REPEATS — isolate COUNT so slide-skip is the measured win. */
putenv('FRACTAL_ZIP_FSS_COUNT=0');
require_once $fz . '/fractal_zip.php';
$raw = file_get_contents($langref);
putenv('FRACTAL_ZIP_FSS_REPEATS=0');
$fz0 = new fractal_zip(256, false, false, null, false);
$t0 = hrtime(true);
$fz0->all_substrings_count($raw);
$off = (hrtime(true) - $t0) / 1e6;
putenv('FRACTAL_ZIP_FSS_REPEATS=1');
$fz1 = new fractal_zip(256, false, false, null, false);
$t0 = hrtime(true);
$fz1->all_substrings_count($raw);
$on = (hrtime(true) - $t0) / 1e6;
printf("REPEATS langref-full legacy=%.0fms hybrid=%.0fms speedup=%.2fx\n",
	$off, $on, $off / max($on, 0.001));

/* HASTOK */
$py = <<<'PY'
import struct, time, subprocess, os
raw=open("/srv/http/fractal_zip/test_files109/enwik8","rb").read(4<<20)
td="/srv/http/fractal_substring/.tmp/real_ab"; os.makedirs(td,exist_ok=True)
open(td+"/c.bin","wb").write(raw)
toks=[]
for i in range(2500):
 o=(i*9973)%(len(raw)-12); toks.append(raw[o:o+4+(i%9)])
for i in range(2500): toks.append(f"zz{i:04d}".encode())
req=struct.pack("<I",len(toks))+b"".join(struct.pack("<I",len(t))+t for t in toks)
open(td+"/r.bin","wb").write(req)
def run(b):
 times=[]
 for _ in range(5):
  t0=time.perf_counter()
  subprocess.check_call([b,td+"/c.bin"],stdin=open(td+"/r.bin","rb"),stdout=open(td+"/o.bin","wb"))
  times.append((time.perf_counter()-t0)*1000)
 return min(times)
h=run("/srv/http/fractal_zip/tools/fractal_compute/hastok")
f=run("/srv/http/fractal_substring/tools/fss")
print(f"HASTOK enwik4MiB+5k hastok={h:.1f}ms fss={f:.1f}ms speedup={h/f:.2f}x")
PY;
file_put_contents($root . '/.tmp/score_hastok.py', $py);
passthru('python3 ' . escapeshellarg($root . '/.tmp/score_hastok.py'));

/* Multi-file peel */
$folderBench = escapeshellarg($root . '/bench/ab_folder_peel.php');
$corp = escapeshellarg($root . '/.tmp/peel_ab_corp');
if (is_dir($root . '/.tmp/peel_ab_corp')) {
	passthru((PHP_BINARY !== '' ? escapeshellarg(PHP_BINARY) : 'php')
		. (extension_loaded('fss') && is_file($ext)
			? ' -d extension=' . escapeshellarg($ext) : '')
		. ' ' . $folderBench . ' --with-fss ' . $corp);
}

/* E2E peel slice */
$peel = substr($raw, 0, 32768);
putenv('FRACTAL_ZIP_FSS=0');
$fzOff = new fractal_zip(256, false, false, null, false);
$t0 = hrtime(true);
$fzOff->recursive_fractal_substring($peel, $peel);
$peelOff = (hrtime(true) - $t0) / 1e6;
putenv('FRACTAL_ZIP_FSS=1');
putenv('FRACTAL_ZIP_FSS_HASTOK=1');
putenv('FRACTAL_ZIP_FSS_COUNT=1');
putenv('FRACTAL_ZIP_FSS_REPEATS=1');
$fzOn = new fractal_zip(256, false, false, null, false);
$t0 = hrtime(true);
$fzOn->recursive_fractal_substring($peel, $peel);
$peelOn = (hrtime(true) - $t0) / 1e6;
printf("PEEL langref-32KiB FSS=0:%.0fms FSS=1:%.0fms speedup=%.2fx\n",
	$peelOff, $peelOn, $peelOff / max($peelOn, 0.001));

/* CLI zip wall (SPEED=1) — roundtrip KEEP */
if (is_file($root . '/bench/ab_zip_folder.sh') && is_dir($root . '/.tmp/peel_ab_corp')) {
	passthru('bash ' . escapeshellarg($root . '/bench/ab_zip_folder.sh')
		. ' ' . escapeshellarg($root . '/.tmp/peel_ab_corp'));
}

/* Peel+pack: ASC-bound path that zip cares about */
if (is_file($root . '/bench/ab_peel_pack.php') && is_dir($root . '/.tmp/peel_ab_corp')) {
	passthru((PHP_BINARY !== '' ? escapeshellarg(PHP_BINARY) : 'php')
		. (extension_loaded('fss') && is_file($ext)
			? ' -d extension=' . escapeshellarg($ext) : '')
		. ' ' . escapeshellarg($root . '/bench/ab_peel_pack.php') . ' --with-fss');
}

echo "LOM: short closers memchr dual/triple ~2–30× vs memmem (see docs/KEEP.md)\n";
echo "done\n";
