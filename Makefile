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
OCC_FLAGS := -Isrc -D_DEFAULT_SOURCE -DOCC_INSTALL_INCLUDE_DIR='"$(PREFIX)/lib/occ/include"'

build/stage2/occ: occ $(SRCS)
	@mkdir -p build/stage2
	./occ -q -o prod -n $@ $(OCC_FLAGS) $(SRCS)

# Self-hosting check: the stage-2 compiler must pass the test suite and emit
# exactly the same assembly as the stage-1 compiler (a bootstrap fixpoint).
selfhost: build/stage2/occ
	@cp build/stage2/occ ./occ-stage2
	@./tests/run.sh ./occ-stage2
	@rm -rf build/fixpoint && mkdir -p build/fixpoint/stage1 build/fixpoint/stage2
	@for f in $(SRCS); do \
	  n=$$(echo $$f | tr / _); \
	  ./occ -q -o prod -S $(OCC_FLAGS) -n build/fixpoint/stage1/$$n.s $$f || exit 1; \
	  ./occ-stage2 -q -o prod -S $(OCC_FLAGS) -n build/fixpoint/stage2/$$n.s $$f || exit 1; \
	done
	@diff -r build/fixpoint/stage1 build/fixpoint/stage2 >/dev/null && \
	  echo "selfhost: stage 1 and stage 2 generate identical assembly (fixpoint reached)"

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
