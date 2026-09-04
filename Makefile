CC      ?= gcc
CFLAGS  ?= -O3 -march=native -std=c11 -Wall -Wextra -Wno-unused-function
CFLAGS  += -Iinclude -Isrc -D_GNU_SOURCE
LDFLAGS ?=
LDLIBS  ?= -lm

SRC = src/fss_find.c src/fss_profile.c src/fss_batch.c src/fss_repeats.c src/fss_sa.c src/fss_store.c
OBJ = $(SRC:.c=.o)

PREFIX ?= /usr/local

.PHONY: all clean test bench install php-ext link-hastok smoke

all: libfss.a libfss.so tools/fss tests/test_fss

php-ext: libfss.so
	cd bindings/php_ext && phpize >/dev/null && \
	  ./configure --enable-fss \
	    CPPFLAGS="-I$(CURDIR)/include" \
	    LDFLAGS="-L$(CURDIR) -Wl,-rpath,$(CURDIR)" >/dev/null && \
	  $(MAKE) -j$$(nproc)
	@echo "Built bindings/php_ext/modules/fss.so"
	@echo "Load: php -d extension=$(CURDIR)/bindings/php_ext/modules/fss.so"

install: libfss.a libfss.so tools/fss
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include $(DESTDIR)$(PREFIX)/bin
	install -m 644 libfss.a $(DESTDIR)$(PREFIX)/lib/
	install -m 755 libfss.so $(DESTDIR)$(PREFIX)/lib/
	install -m 644 include/fss.h $(DESTDIR)$(PREFIX)/include/
	install -m 644 include/fmem.h include/fcache.h $(DESTDIR)$(PREFIX)/include/ 2>/dev/null || true
	install -m 755 tools/fss $(DESTDIR)$(PREFIX)/bin/fss
	@echo "Installed to $(DESTDIR)$(PREFIX); PHP ext stays in-tree (make php-ext)."

smoke: tools/fss
	./tests/test_fss
	$(MAKE) php-ext
	TMPDIR=$(CURDIR)/.tmp php -d extension=$(CURDIR)/bindings/php_ext/modules/fss.so tests/verify_integrate.php
	TMPDIR=$(CURDIR)/.tmp php -d extension=$(CURDIR)/bindings/php_ext/modules/fss.so tests/smoke_peel_fss.php
	TMPDIR=$(CURDIR)/.tmp php bench/ab_repeats_immense.php 1048576
	bash bench/ensure_peel_corpus.sh
	TMPDIR=$(CURDIR)/.tmp bash bench/ab_zip_folder.sh
	TMPDIR=$(CURDIR)/.tmp php -d extension=$(CURDIR)/bindings/php_ext/modules/fss.so bench/ab_peel_pack.php --with-fss
	@echo "smoke ok"
libfss.a: $(OBJ)
	ar rcs $@ $^

libfss.so: $(OBJ)
	$(CC) -shared -fPIC -o $@ $^ $(LDLIBS)

src/%.o: src/%.c include/fss.h src/fss_internal.h
	$(CC) $(CFLAGS) -fPIC -c -o $@ $<

tools/fss: tools/fss.c libfss.a
	$(CC) $(CFLAGS) -o $@ tools/fss.c libfss.a $(LDLIBS)

tests/test_fss: tests/test_fss.c libfss.a
	$(CC) $(CFLAGS) -o $@ tests/test_fss.c libfss.a $(LDLIBS)

bench/bench_fss: bench/bench_fss.c libfss.a
	$(CC) $(CFLAGS) -o $@ bench/bench_fss.c libfss.a $(LDLIBS)

test: tests/test_fss
	./tests/test_fss

bench: bench/bench_fss
	./bench/bench_fss

# Point fractal_zip at this binary for hastok-compatible presence:
#   FRACTAL_ZIP_HASTOK_BIN=/srv/http/fractal_substring/tools/fss
# or: make link-hastok
link-hastok: tools/fss
	ln -sfn /srv/http/fractal_substring/tools/fss \
	  /srv/http/fractal_zip/tools/fractal_compute/fss
	@echo "Set FRACTAL_ZIP_HASTOK_BIN to tools/fss or use FRACTAL_ZIP_FSS_HASTOK=1"

clean:
	rm -f $(OBJ) libfss.a libfss.so tools/fss tests/test_fss bench/bench_fss
