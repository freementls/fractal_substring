#!/usr/bin/env php
<?php
/**
 * A/B: fss_php_count vs substr_count must match; find vs strpos must match.
 */
declare(strict_types=1);

$root = dirname(__DIR__);
require_once $root . '/bindings/fss.php';

putenv('FRACTAL_ZIP_FSS_COUNT=1');
putenv('FRACTAL_ZIP_FSS=1');

$haystacks = array(
	'',
	'a',
	str_repeat('abc', 1000),
	str_repeat('the quick brown fox ', 5000),
	random_bytes(4096),
);
$needles = array('a', 'abc', 'fox', 'zzz', 'the ', "\0\0");

$fail = 0;
foreach ($haystacks as $hi => $h) {
	foreach ($needles as $ni => $nd) {
		if ($nd === '') {
			continue;
		}
		$want = substr_count($h, $nd);
		$got = fss_php_count($h, $nd);
		if ($want !== $got) {
			fwrite(STDERR, "count mismatch h$hi n$ni want=$want got=$got\n");
			$fail++;
		}
		$wp = strpos($h, $nd);
		$gp = fss_php_find($h, $nd);
		if ($wp !== $gp) {
			fwrite(STDERR, "find mismatch h$hi n$ni want=" . var_export($wp, true)
				. ' got=' . var_export($gp, true) . "\n");
			$fail++;
		}
	}
}

/* Direct C CLI count check */
$tmp = tempnam(sys_get_temp_dir(), 'fss');
file_put_contents($tmp, str_repeat('hello ', 1000) . 'world');
$bin = $root . '/tools/fss';
$out = is_file($bin)
	? trim((string) shell_exec(escapeshellarg($bin) . ' count ' . escapeshellarg($tmp) . ' hello'))
	: '';
@unlink($tmp);
$php = substr_count(str_repeat('hello ', 1000) . 'world', 'hello');
if ($out !== '' && (int) $out !== $php) {
	fwrite(STDERR, "cli count mismatch cli=$out php=$php\n");
	$fail++;
}

if ($fail) {
	fwrite(STDERR, "$fail failure(s)\n");
	exit(1);
}
echo "verify-integrate ok\n";
