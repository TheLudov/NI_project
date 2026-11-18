# INFO8011 L4 load balancer: builds your LB.  Provided: do not modify it (the submission platform uses its own copy).
#
#   make            build/lb and build/autotune
#   make debug      the same in build-debug/, with AddressSanitizer and UndefinedBehaviorSanitizer
#   make test       builds, then runs the test suite (tests/run.sh; tests/run.sh build-debug after make debug)
#   make clean
#
# Your components: every .c file under src/ (subfolders allowed), compiled into build/libcomp.a.
# build/lb is lb/main.c, build/autotune is autotune/main.c, each linked with libcomp.a and the harness
# (gen/<arch>/libgen.a).  Include paths: src/ and gen/.  Nothing is taken from the environment but CC.
#
# Linux on x86-64 or aarch64, GCC or Clang.

ARCH := $(shell uname -m)
ifeq ($(ARCH),x86_64)
  ARCHFLAGS := -msse4.2
  GENLIB    := gen/x86_64/libgen.a
else ifneq ($(filter aarch64 arm64,$(ARCH)),)
  ARCHFLAGS := -march=armv8-a+crc
  GENLIB    := gen/aarch64/libgen.a
else
  $(error unsupported architecture $(ARCH): x86_64 or aarch64)
endif

FLAGS     := -std=c11 -O2 -Wall -Wextra $(ARCHFLAGS) -Isrc -Igen
DBG_FLAGS := -std=c11 -O1 -g -Wall -Wextra $(ARCHFLAGS) -Isrc -Igen -fsanitize=address,undefined
LIBS      := -lpthread -lm

COMP_SRCS := $(shell find src -name '*.c' 2>/dev/null)
HDRS      := $(shell find src -name '*.h' 2>/dev/null) gen/gen.h gen/crc32c.h

all: build/lb build/autotune

debug: build-debug/lb build-debug/autotune

# the rules for one build directory ($1) and its flags ($2)
define PROGRAMS
$(1)/obj/%.o: %.c $$(HDRS)
	@mkdir -p $$(@D)
	$$(CC) $(2) -c $$< -o $$@
# rebuilt when a source file comes or goes; with no source file, an empty archive
$(1)/comp.list: FORCE
	@mkdir -p $$(@D)
	@echo '$$(COMP_SRCS)' | cmp -s - $$@ || echo '$$(COMP_SRCS)' > $$@
$(1)/libcomp.a: $(1)/comp.list $$(COMP_SRCS:%.c=$(1)/obj/%.o)
	rm -f $$@ && $$(if $$(COMP_SRCS),ar rcs $$@ $$(COMP_SRCS:%.c=$(1)/obj/%.o),printf '!<arch>\n' > $$@)
$(1)/lb: $(1)/obj/lb/main.o $(1)/libcomp.a $$(GENLIB)
	$$(CC) $(2) $$^ $$(LIBS) -o $$@
$(1)/autotune: $(1)/obj/autotune/main.o $(1)/libcomp.a $$(GENLIB)
	$$(CC) $(2) $$^ $$(LIBS) -o $$@
endef
$(eval $(call PROGRAMS,build,$(FLAGS)))
$(eval $(call PROGRAMS,build-debug,$(DBG_FLAGS)))

build/test_crc32c: tests/test_crc32c.c gen/crc32c.h
	@mkdir -p $(@D)
	$(CC) $(FLAGS) $< -o $@

test: all build/test_crc32c
	@./build/test_crc32c
	@tests/run.sh build

clean:
	rm -rf build build-debug

.PHONY: all debug test clean FORCE
