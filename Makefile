# occ - the Opus C Compiler
#
#   make            build ./occ
#   make test       run the test suite (every test at -o none and -o prod)
#   make selfhost   compile occ with itself (stage 2) and run the tests with it
#   make bench      compare generated code speed
#   make install    install to $(PREFIX) (default /usr/local)

CC      ?= cc
PREFIX  ?= /usr/local

# Prefer -std=c23; older GCC/Clang spell it -std=c2x.
STD     := $(shell $(CC) -std=c23 -E -x c /dev/null >/dev/null 2>&1 && echo c23 || echo c2x)

CFLAGS  ?= -O2 -g
CFLAGS  += -std=$(STD) -Wall -Wextra -Wshadow -Wno-unused-parameter -Wno-sign-compare
CPPFLAGS += -Isrc -D_DEFAULT_SOURCE -DOCC_INSTALL_INCLUDE_DIR='"$(PREFIX)/lib/occ/include"'

SRCS    := $(sort $(shell find src -name '*.c'))
OBJS    := $(SRCS:src/%.c=build/%.o)
DEPS    := $(OBJS:.o=.d)

.PHONY: all test test-stage2 selfhost bench install uninstall clean format

all: occ

occ: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c -o $@ $<

test: occ
	@./tests/run.sh ./occ

# Stage 2: occ compiled by occ (with optimizations enabled).
build/stage2/occ: occ $(SRCS)
	@mkdir -p build/stage2
	./occ -q -o prod -n $@ -Isrc -D_DEFAULT_SOURCE \
	  -DOCC_INSTALL_INCLUDE_DIR='"$(PREFIX)/lib/occ/include"' $(SRCS)

selfhost: build/stage2/occ
	@cp build/stage2/occ ./occ-stage2
	@./tests/run.sh ./occ-stage2

bench: occ
	@./tests/bench/run.sh ./occ

install: occ
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/occ/include
	install -m 755 occ $(DESTDIR)$(PREFIX)/bin/occ
	install -m 644 include/*.h $(DESTDIR)$(PREFIX)/lib/occ/include/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/occ
	rm -rf $(DESTDIR)$(PREFIX)/lib/occ

clean:
	rm -rf build occ occ-stage2 tests/out

format:
	clang-format -i $(SRCS) $(shell find src -name '*.h')

-include $(DEPS)
