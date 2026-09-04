PHP_ARG_ENABLE(fss, whether to enable fractal substring support,
[  --enable-fss           Enable libfss substring extension])

if test "$PHP_FSS" != "no"; then
  PHP_ADD_INCLUDE([../../include])
  PHP_ADD_LIBRARY_WITH_PATH(fss, ../../, FSS_SHARED_LIBADD)
  PHP_SUBST(FSS_SHARED_LIBADD)
  PHP_NEW_EXTENSION(fss, php_fss.c, $ext_shared,, -DZEND_ENABLE_STATIC_TSRMLS_CACHE=1)
fi
