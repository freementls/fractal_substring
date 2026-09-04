#!/usr/bin/env php
<?php
/**
 * Immense repeats: CDC/block path wall on large repetitive vs low-redundancy.
 * Usage: php bench/ab_repeats_immense.php [bytes=2097152]
 */
declare(strict_types=1);

$n = isset($argv[1]) ? (int) $argv[1] : 2 * 1024 * 1024;
if ($n < 256 * 1024) {
	$n = 256 * 1024;
}
$bin = dirname(__DIR__) . '/tools/fss';
if (!is_executable($bin)) {
	fwrite(STDERR, "build tools/fss first\n");
	exit(2);
}

function make_rep(int $n): string {
	$tile = str_repeat('<div class="row">item TOKEN</div>', 64); // ~2KiB-ish
	$out = '';
	while (strlen($out) < $n) {
		$out .= $tile;
	}
	return substr($out, 0, $n);
}

function make_low(int $n): string {
	$s = random_bytes($n);
	$tile = 4096;
	$a = 0;
	$b = 2 * 65536; /* distant 64 KiB blocks */
	if ($n > $b + $tile) {
		$s = substr_replace($s, substr($s, $a, $tile), $b, $tile);
	}
	return $s;
}

function time_repeats(string $bin, string $raw, int $min, int $max, int $topk): array {
	$tmp = tempnam(sys_get_temp_dir(), 'fss_imm');
	file_put_contents($tmp, $raw);
	$t0 = hrtime(true);
	$out = [];
	$code = 0;
	exec(escapeshellarg($bin) . ' repeats ' . escapeshellarg($tmp)
		. " $min $max $topk 2>/dev/null", $out, $code);
	$ms = (hrtime(true) - $t0) / 1e6;
	@unlink($tmp);
	return [$ms, count($out), $code];
}

$rep = make_rep($n);
$low = make_low($n);
echo "bytes=$n\n";

[$msR, $nrR, $cR] = time_repeats($bin, $rep, 8, 4096, 16);
[$msL, $nrL, $cL] = time_repeats($bin, $low, 8, 4096, 16);

echo sprintf("repetitive: %.1fms candidates=%d code=%d\n", $msR, $nrR, $cR);
echo sprintf("low+twin:   %.1fms candidates=%d code=%d\n", $msL, $nrL, $cL);

/* KEEP: finish in under ~2s/MiB on this machine class; find ≥1 cand on both. */
$perMiB = $msR / ($n / (1024 * 1024));
$keep = ($cR === 0 && $cL === 0 && $nrR >= 1 && $nrL >= 1 && $perMiB < 2000);
echo sprintf("ms_per_MiB_rep=%.1f\n", $perMiB);
echo $keep ? "KEEP immense repeats: PASS\n" : "KEEP immense repeats: FAIL\n";
exit($keep ? 0 : 1);
