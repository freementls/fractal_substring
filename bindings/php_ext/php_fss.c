/*
 * php_fss — thin Zend wrapper around libfss.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "php.h"
#include "ext/standard/info.h"
#include "fss.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- arginfo (PHP 8+) ---- */
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_MASK_EX(arginfo_fss_ext_find, 0, 2, MAY_BE_LONG|MAY_BE_FALSE)
	ZEND_ARG_TYPE_INFO(0, haystack, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, needle, IS_STRING, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fss_ext_count, 0, 2, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, haystack, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, needle, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, overlap, _IS_BOOL, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fss_ext_count_batch, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, haystack, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, needles, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fss_ext_has_batch, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, haystack, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, needles, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fss_ext_repeats, 0, 1, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, haystack, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, min_len, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, max_len, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, top_k, IS_LONG, 0)
	ZEND_ARG_TYPE_INFO(0, min_count, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_fss_ext_find_all, 0, 2, IS_ARRAY, 0)
	ZEND_ARG_TYPE_INFO(0, haystack, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, needle, IS_STRING, 0)
	ZEND_ARG_TYPE_INFO(0, overlap, _IS_BOOL, 0)
	ZEND_ARG_TYPE_INFO(0, cap, IS_LONG, 0)
ZEND_END_ARG_INFO()

PHP_MINFO_FUNCTION(fss)
{
	php_info_print_table_start();
	php_info_print_table_row(2, "fss (libfss)", "enabled");
	php_info_print_table_end();
}

PHP_FUNCTION(fss_ext_find)
{
	zend_string *hay, *nd;
	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(hay)
		Z_PARAM_STR(nd)
	ZEND_PARSE_PARAMETERS_END();

	ssize_t off = fss_find((const uint8_t *)ZSTR_VAL(hay), ZSTR_LEN(hay),
	                       (const uint8_t *)ZSTR_VAL(nd), ZSTR_LEN(nd));
	if (off < 0) {
		RETURN_FALSE;
	}
	RETURN_LONG((zend_long)off);
}

PHP_FUNCTION(fss_ext_count)
{
	zend_string *hay, *nd;
	zend_bool overlap = 0;
	ZEND_PARSE_PARAMETERS_START(2, 3)
		Z_PARAM_STR(hay)
		Z_PARAM_STR(nd)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(overlap)
	ZEND_PARSE_PARAMETERS_END();

	size_t c = fss_count((const uint8_t *)ZSTR_VAL(hay), ZSTR_LEN(hay),
	                     (const uint8_t *)ZSTR_VAL(nd), ZSTR_LEN(nd),
	                     overlap ? 1 : 0);
	RETURN_LONG((zend_long)c);
}

PHP_FUNCTION(fss_ext_count_batch)
{
	zend_string *hay;
	zval *needles;
	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(hay)
		Z_PARAM_ARRAY(needles)
	ZEND_PARSE_PARAMETERS_END();

	uint32_t n = zend_hash_num_elements(Z_ARRVAL_P(needles));
	const uint8_t **nds = NULL;
	size_t *ms = NULL;
	uint32_t *out = NULL;
	zend_string **owned = NULL;

	if (n) {
		nds = ecalloc(n, sizeof(*nds));
		ms = ecalloc(n, sizeof(*ms));
		out = ecalloc(n, sizeof(*out));
		owned = ecalloc(n, sizeof(*owned));
	}

	uint32_t i = 0;
	zval *val;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(needles), val) {
		zend_string *s = zval_get_string(val);
		owned[i] = s;
		nds[i] = (const uint8_t *)ZSTR_VAL(s);
		ms[i] = ZSTR_LEN(s);
		i++;
		if (i >= n) break;
	} ZEND_HASH_FOREACH_END();

	fss_count_batch((const uint8_t *)ZSTR_VAL(hay), ZSTR_LEN(hay),
	                nds, ms, n, 0, out);

	array_init_size(return_value, n);
	for (i = 0; i < n; i++) {
		add_next_index_long(return_value, (zend_long)out[i]);
		zend_string_release(owned[i]);
	}
	efree(nds);
	efree(ms);
	efree(out);
	efree(owned);
}

PHP_FUNCTION(fss_ext_has_batch)
{
	zend_string *hay;
	zval *needles;
	ZEND_PARSE_PARAMETERS_START(2, 2)
		Z_PARAM_STR(hay)
		Z_PARAM_ARRAY(needles)
	ZEND_PARSE_PARAMETERS_END();

	uint32_t n = zend_hash_num_elements(Z_ARRVAL_P(needles));
	const uint8_t **nds = NULL;
	size_t *ms = NULL;
	uint8_t *out = NULL;
	zend_string **owned = NULL;

	if (n) {
		nds = ecalloc(n, sizeof(*nds));
		ms = ecalloc(n, sizeof(*ms));
		out = ecalloc(n, 1);
		owned = ecalloc(n, sizeof(*owned));
	}

	uint32_t i = 0;
	zval *val;
	ZEND_HASH_FOREACH_VAL(Z_ARRVAL_P(needles), val) {
		zend_string *s = zval_get_string(val);
		owned[i] = s;
		nds[i] = (const uint8_t *)ZSTR_VAL(s);
		ms[i] = ZSTR_LEN(s);
		i++;
		if (i >= n) break;
	} ZEND_HASH_FOREACH_END();

	fss_has_batch((const uint8_t *)ZSTR_VAL(hay), ZSTR_LEN(hay),
	              nds, ms, n, out);

	array_init_size(return_value, n);
	for (i = 0; i < n; i++) {
		add_next_index_long(return_value, out[i] ? 1 : 0);
		zend_string_release(owned[i]);
	}
	efree(nds);
	efree(ms);
	efree(out);
	efree(owned);
}

PHP_FUNCTION(fss_ext_repeats)
{
	zend_string *hay;
	zend_long min_len = 4, max_len = 256, top_k = 24, min_count = 2;
	ZEND_PARSE_PARAMETERS_START(1, 5)
		Z_PARAM_STR(hay)
		Z_PARAM_OPTIONAL
		Z_PARAM_LONG(min_len)
		Z_PARAM_LONG(max_len)
		Z_PARAM_LONG(top_k)
		Z_PARAM_LONG(min_count)
	ZEND_PARSE_PARAMETERS_END();

	if (min_len < 1) min_len = 1;
	if (max_len < min_len) max_len = min_len;
	if (top_k < 1) top_k = 1;
	if (top_k > 64) top_k = 64;
	if (min_count < 2) min_count = 2;

	fss_repeat_opts o = {
		.min_len = (size_t)min_len,
		.max_len = (size_t)max_len,
		.min_count = (uint32_t)min_count,
		.top_k = (size_t)top_k,
		.marker_len_base = 0,
	};
	fss_repeat *out = ecalloc((size_t)top_k, sizeof(fss_repeat));
	size_t nr = fss_repeats((const uint8_t *)ZSTR_VAL(hay), ZSTR_LEN(hay),
	                        &o, out, (size_t)top_k);

	array_init_size(return_value, (uint32_t)nr);
	for (size_t i = 0; i < nr; i++) {
		if (!out[i].p || out[i].len == 0 || out[i].count < 2) continue;
		zend_string *key = zend_string_init((const char *)out[i].p, out[i].len, 0);
		add_assoc_long_ex(return_value, ZSTR_VAL(key), ZSTR_LEN(key),
		                  (zend_long)out[i].count);
		zend_string_release(key);
	}
	efree(out);
}

PHP_FUNCTION(fss_ext_find_all)
{
	zend_string *hay, *nd;
	zend_bool overlap = 0;
	zend_long cap = 65536;
	ZEND_PARSE_PARAMETERS_START(2, 4)
		Z_PARAM_STR(hay)
		Z_PARAM_STR(nd)
		Z_PARAM_OPTIONAL
		Z_PARAM_BOOL(overlap)
		Z_PARAM_LONG(cap)
	ZEND_PARSE_PARAMETERS_END();

	if (cap < 1) cap = 1;
	if (cap > 1048576) cap = 1048576;

	size_t *offs = ecalloc((size_t)cap, sizeof(size_t));
	size_t n = fss_find_all((const uint8_t *)ZSTR_VAL(hay), ZSTR_LEN(hay),
	                        (const uint8_t *)ZSTR_VAL(nd), ZSTR_LEN(nd),
	                        overlap ? 1 : 0, offs, (size_t)cap);
	array_init_size(return_value, (uint32_t)n);
	for (size_t i = 0; i < n; i++) {
		add_next_index_long(return_value, (zend_long)offs[i]);
	}
	efree(offs);
}

zend_function_entry fss_functions[] = {
	PHP_FE(fss_ext_find, arginfo_fss_ext_find)
	PHP_FE(fss_ext_count, arginfo_fss_ext_count)
	PHP_FE(fss_ext_count_batch, arginfo_fss_ext_count_batch)
	PHP_FE(fss_ext_has_batch, arginfo_fss_ext_has_batch)
	PHP_FE(fss_ext_repeats, arginfo_fss_ext_repeats)
	PHP_FE(fss_ext_find_all, arginfo_fss_ext_find_all)
	PHP_FE_END
};

zend_module_entry fss_module_entry = {
	STANDARD_MODULE_HEADER,
	"fss",
	fss_functions,
	NULL,
	NULL,
	NULL,
	NULL,
	PHP_MINFO(fss),
	"0.1.1",
	STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_FSS
# ifdef ZTS
ZEND_TSRMLS_CACHE_DEFINE()
# endif
ZEND_GET_MODULE(fss)
#endif
