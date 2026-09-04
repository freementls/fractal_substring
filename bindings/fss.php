<?php

declare(strict_types=1);

/**
 * Shared PHP adapter for libfss (fractal substring search).
 *
 * Pure accelerator: every public helper falls through to PHP builtins when
 * the library/binary is missing, FFI is unavailable, or the site kill switch
 * is off. Answers must match substr_count / strpos / str_contains.
 *
 * Env:
 *   FRACTAL_ZIP_FSS=0          hard off (all sites)
 *   FRACTAL_ZIP_FSS_COUNT=0    disable COUNT (default ON if fss.so loads)
 *   FRACTAL_ZIP_FSS_HASTOK=0   classic hastok (default ON if tools/fss)
 *   FRACTAL_ZIP_FSS_REPEATS=0  legacy ASC slide only (default ON hybrid)
 *   FRACTAL_ZIP_FSS_LIB=path   override libfss.so
 *   FRACTAL_ZIP_FSS_BIN=path   override tools/fss
 *   LOM_FSS=0 / LOM_FSS_SCAN=0 similarly for LOM (see lom bindings)
 */

function fss_php_root(): string
{
	return dirname(__DIR__);
}

function fss_php_global_enabled(): bool
{
	$v = getenv('FRACTAL_ZIP_FSS');
	if ($v === false || trim((string) $v) === '') {
		/* Default: library may be used when per-site flags say so. */
		return true;
	}
	return !in_array(strtolower(trim((string) $v)), array('0', 'false', 'off', 'no'), true);
}

function fss_php_ext_path(): ?string
{
	static $resolved = false;
	static $path = null;
	if ($resolved) {
		return $path;
	}
	$resolved = true;
	$env = trim((string) getenv('FRACTAL_ZIP_FSS_EXT'));
	$cands = array();
	if ($env !== '') {
		$cands[] = $env;
	}
	$root = fss_php_root();
	$cands[] = $root . '/bindings/php_ext/modules/fss.so';
	foreach ($cands as $c) {
		if (is_file($c) && is_readable($c)) {
			$path = $c;
			return $path;
		}
	}
	return null;
}

/** Ensure the native fss extension is loaded (dlopen once). */
function fss_php_ensure_ext(): bool
{
	static $ok = null;
	if ($ok !== null) {
		return $ok;
	}
	if (extension_loaded('fss') && function_exists('fss_ext_count')) {
		$ok = true;
		return true;
	}
	$path = fss_php_ext_path();
	if ($path === null) {
		$ok = false;
		return false;
	}
	/* Prefer process-local load without touching php.ini. */
	if (function_exists('dl')) {
		$prev = set_error_handler(static function () {
			return true;
		});
		@dl($path);
		if ($prev) {
			set_error_handler($prev);
		} else {
			restore_error_handler();
		}
	}
	/* Fallback: some builds disable dl(); still works if user -d extension= */
	$ok = extension_loaded('fss') && function_exists('fss_ext_count');
	return $ok;
}

function fss_php_site_enabled(string $site): bool
{
	if (!fss_php_global_enabled()) {
		return false;
	}
	$env = 'FRACTAL_ZIP_FSS_' . strtoupper($site);
	$v = getenv($env);
	if ($v === false || trim((string) $v) === '') {
		$siteU = strtoupper($site);
		if ($siteU === 'HASTOK') {
			return fss_php_bin_path() !== null;
		}
		/* COUNT default-ON when native extension is available (KEEP ~2× batch). */
		if ($siteU === 'COUNT') {
			return fss_php_ensure_ext();
		}
		/* REPEATS default-ON when ext or tools/fss exists — merge seeds + skip slide. */
		if ($siteU === 'REPEATS') {
			return fss_php_ensure_ext() || fss_php_bin_path() !== null;
		}
		return false;
	}
	return !in_array(strtolower(trim((string) $v)), array('0', 'false', 'off', 'no'), true);
}

function fss_php_lib_path(): ?string
{
	static $resolved = false;
	static $path = null;
	if ($resolved) {
		return $path;
	}
	$resolved = true;
	$env = trim((string) getenv('FRACTAL_ZIP_FSS_LIB'));
	$cands = array();
	if ($env !== '') {
		$cands[] = $env;
	}
	$root = fss_php_root();
	$cands[] = $root . '/libfss.so';
	$cands[] = $root . '/bindings/../libfss.so';
	foreach ($cands as $c) {
		if (is_file($c) && is_readable($c)) {
			$path = $c;
			return $path;
		}
	}
	return null;
}

function fss_php_bin_path(): ?string
{
	static $resolved = false;
	static $path = null;
	if ($resolved) {
		return $path;
	}
	$resolved = true;
	$env = trim((string) getenv('FRACTAL_ZIP_FSS_BIN'));
	$cands = array();
	if ($env !== '') {
		$cands[] = $env;
	}
	$root = fss_php_root();
	$cands[] = $root . '/tools/fss';
	foreach ($cands as $c) {
		if (is_file($c) && is_executable($c)) {
			$path = $c;
			return $path;
		}
	}
	return null;
}

/**
 * @return FFI|null
 */
function fss_php_ffi()
{
	static $ffi = false; /* false=untried, null=failed, FFI=ok */
	if ($ffi !== false) {
		return $ffi;
	}
	$ffi = null;
	if (!extension_loaded('ffi')) {
		return null;
	}
	$lib = fss_php_lib_path();
	if ($lib === null) {
		return null;
	}
	$cdef = <<<'C'
ssize_t fss_find(const uint8_t *h, size_t n, const uint8_t *nd, size_t m);
size_t  fss_count(const uint8_t *h, size_t n, const uint8_t *nd, size_t m, int overlap);
size_t  fss_find_all(const uint8_t *h, size_t n, const uint8_t *nd, size_t m, int overlap, size_t *offsets, size_t out_cap);
void    fss_has_batch_blob(const uint8_t *h, size_t n,
                           const uint8_t *blob, const uint32_t *offs,
                           const uint32_t *lens, size_t k, uint8_t *out_bits);
C;
	try {
		$ffi = \FFI::cdef($cdef, $lib);
	} catch (Throwable $e) {
		$ffi = null;
	}
	return $ffi;
}

/** Non-overlapping count matching PHP substr_count. */
function fss_php_count(string $haystack, string $needle): int
{
	if ($needle === '') {
		return 0;
	}
	if (!fss_php_site_enabled('COUNT') || !fss_should_delegate(strlen($haystack), strlen($needle), 'count')) {
		return substr_count($haystack, $needle);
	}
	if (fss_php_ensure_ext()) {
		return (int) fss_ext_count($haystack, $needle, false);
	}
	$ffi = fss_php_ffi();
	if ($ffi === null) {
		return substr_count($haystack, $needle);
	}
	$n = strlen($haystack);
	$m = strlen($needle);
	$h = $ffi->new('uint8_t[' . max($n, 1) . ']');
	$nd = $ffi->new('uint8_t[' . max($m, 1) . ']');
	\FFI::memcpy($h, $haystack, $n);
	\FFI::memcpy($nd, $needle, $m);
	return (int) $ffi->fss_count($h, $n, $nd, $m, 0);
}

/** First offset or false — matches strpos. */
function fss_php_find(string $haystack, string $needle)
{
	if ($needle === '') {
		return 0;
	}
	if (!fss_php_global_enabled()) {
		$p = strpos($haystack, $needle);
		return $p === false ? false : $p;
	}
	if (fss_php_ensure_ext()) {
		$off = fss_ext_find($haystack, $needle);
		return $off === false ? false : (int) $off;
	}
	$ffi = fss_php_ffi();
	if ($ffi === null) {
		$p = strpos($haystack, $needle);
		return $p === false ? false : $p;
	}
	$n = strlen($haystack);
	$m = strlen($needle);
	$h = $ffi->new('uint8_t[' . max($n, 1) . ']');
	$nd = $ffi->new('uint8_t[' . max($m, 1) . ']');
	\FFI::memcpy($h, $haystack, $n);
	\FFI::memcpy($nd, $needle, $m);
	$off = (int) $ffi->fss_find($h, $n, $nd, $m);
	return $off < 0 ? false : $off;
}

/**
 * Occurrence offsets (list of ints). overlap=false matches PHP strpos walks.
 *
 * @return list<int>
 */
function fss_php_find_all(string $haystack, string $needle, bool $overlap = false, int $cap = 65536): array
{
	if ($needle === '') {
		return array(0);
	}
	if (!fss_php_global_enabled()) {
		$out = array();
		$off = 0;
		$step = $overlap ? 1 : strlen($needle);
		if ($step < 1) {
			$step = 1;
		}
		while (($p = strpos($haystack, $needle, $off)) !== false) {
			$out[] = $p;
			if (count($out) >= $cap) {
				break;
			}
			$off = $p + $step;
		}
		return $out;
	}
	if (fss_php_ensure_ext() && function_exists('fss_ext_find_all')) {
		/** @var list<int> $a */
		$a = fss_ext_find_all($haystack, $needle, $overlap, $cap);
		return $a;
	}
	$ffi = fss_php_ffi();
	if ($ffi === null) {
		$out = array();
		$off = 0;
		$step = $overlap ? 1 : strlen($needle);
		if ($step < 1) {
			$step = 1;
		}
		while (($p = strpos($haystack, $needle, $off)) !== false) {
			$out[] = $p;
			if (count($out) >= $cap) {
				break;
			}
			$off = $p + $step;
		}
		return $out;
	}
	$n = strlen($haystack);
	$m = strlen($needle);
	if ($cap < 1) {
		$cap = 1;
	}
	if ($cap > 1048576) {
		$cap = 1048576;
	}
	$h = $ffi->new('uint8_t[' . max($n, 1) . ']');
	$nd = $ffi->new('uint8_t[' . max($m, 1) . ']');
	$offs = $ffi->new('size_t[' . $cap . ']');
	\FFI::memcpy($h, $haystack, $n);
	\FFI::memcpy($nd, $needle, $m);
	$c = (int) $ffi->fss_find_all($h, $n, $nd, $m, $overlap ? 1 : 0, $offs, $cap);
	$out = array();
	for ($i = 0; $i < $c; $i++) {
		$out[] = (int) $offs[$i];
	}
	return $out;
}

function fss_should_delegate(int $hayN, int $needleM, string $kind): bool
{
	if ($kind === 'repeats') {
		return $hayN >= 64;
	}
	if ($kind === 'count') {
		/* Single-needle: only when hay is large enough that C wins. */
		return $hayN >= 65536 && $needleM >= 2;
	}
	return true;
}

/**
 * Path to hastok-compatible fss binary when FSS_HASTOK is enabled.
 * Returns null to keep the classic hastok binary.
 */
function fss_php_hastok_bin_override(): ?string
{
	if (!fss_php_site_enabled('HASTOK')) {
		return null;
	}
	return fss_php_bin_path();
}

/**
 * Batch non-overlapping counts via tools/fss count-batch (no FFI required).
 * Returns list of counts aligned with $needles, or null on failure.
 *
 * @param list<string> $needles
 * @return list<int>|null
 */
function fss_php_count_batch(string $haystack, array $needles): ?array
{
	if ($needles === array()) {
		return array();
	}
	if (!fss_php_site_enabled('COUNT')) {
		$out = array();
		foreach ($needles as $nd) {
			$out[] = ($nd === '') ? 0 : substr_count($haystack, (string) $nd);
		}
		return $out;
	}
	/* Prefer in-process extension (no IPC). */
	if (fss_php_ensure_ext()) {
		$needles = array_values($needles);
		$counts = fss_ext_count_batch($haystack, $needles);
		$out = array();
		foreach ($counts as $c) {
			$out[] = (int) $c;
		}
		return $out;
	}
	$bin = fss_php_bin_path();
	if ($bin === null || strlen($haystack) < 64) {
		$out = array();
		foreach ($needles as $nd) {
			$out[] = ($nd === '') ? 0 : substr_count($haystack, (string) $nd);
		}
		return $out;
	}

	$tmp = sys_get_temp_dir() . '/fss_cb_' . getmypid() . '_' . bin2hex(random_bytes(3));
	@mkdir($tmp, 0700, true);
	$hayFile = $tmp . '/hay.bin';
	file_put_contents($hayFile, $haystack);

	$req = pack('V', count($needles));
	foreach ($needles as $nd) {
		$nd = (string) $nd;
		$req .= pack('V', strlen($nd)) . $nd;
	}

	$desc = array(0 => array('pipe', 'r'), 1 => array('pipe', 'w'), 2 => array('pipe', 'w'));
	$proc = @proc_open(escapeshellarg($bin) . ' count-batch ' . escapeshellarg($hayFile), $desc, $pipes);
	$raw = '';
	if (is_resource($proc)) {
		fwrite($pipes[0], $req);
		fclose($pipes[0]);
		$raw = (string) stream_get_contents($pipes[1]);
		fclose($pipes[1]);
		fclose($pipes[2]);
		proc_close($proc);
	}
	@unlink($hayFile);
	@rmdir($tmp);

	$need = count($needles) * 4;
	if (strlen($raw) !== $need) {
		return null;
	}
	$out = array();
	for ($i = 0; $i < count($needles); $i++) {
		$u = unpack('V', substr($raw, $i * 4, 4));
		$out[] = (int) $u[1];
	}
	return $out;
}

/**
 * Repeat discovery: prefer in-process ext, else tools/fss CLI.
 *
 * @return array<string,int>|null substr => count
 */
function fss_php_repeats_map(string $haystack, int $minLen = 4, int $maxLen = 256, int $topK = 24): ?array
{
	if (!fss_php_site_enabled('REPEATS') || !fss_should_delegate(strlen($haystack), 0, 'repeats')) {
		return null;
	}
	if (fss_php_ensure_ext() && function_exists('fss_ext_repeats')) {
		$raw = fss_ext_repeats($haystack, $minLen, $maxLen, $topK, 2);
		if (!is_array($raw) || $raw === array()) {
			return null;
		}
		$map = array();
		foreach ($raw as $sub => $cnt) {
			$sub = (string) $sub;
			$cnt = (int) $cnt;
			if ($sub !== '' && $cnt >= 2) {
				$map[$sub] = $cnt;
			}
		}
		return $map === array() ? null : $map;
	}
	$bin = fss_php_bin_path();
	if ($bin === null) {
		return null;
	}
	$tmp = sys_get_temp_dir() . '/fss_rep_' . getmypid() . '_' . bin2hex(random_bytes(3));
	file_put_contents($tmp, $haystack);
	$out = array();
	$code = 0;
	exec(escapeshellarg($bin) . ' repeats ' . escapeshellarg($tmp)
		. ' ' . (int) $minLen . ' ' . (int) $maxLen . ' ' . (int) $topK
		. ' 2>/dev/null', $out, $code);
	@unlink($tmp);
	if ($code !== 0) {
		return null;
	}
	$map = array();
	foreach ($out as $line) {
		$parts = explode("\t", $line, 4);
		if (count($parts) < 4) {
			continue;
		}
		$count = (int) $parts[1];
		$len = (int) $parts[2];
		$raw = base64_decode($parts[3], true);
		if ($raw === false || strlen($raw) !== $len) {
			$raw = $parts[3];
		}
		if ($raw !== '' && $count >= 2) {
			$map[$raw] = $count;
		}
	}
	return $map === array() ? null : $map;
}

