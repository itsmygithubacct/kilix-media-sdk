# kilix-acoustic-link — component build.
#
# Offline by construction: every dependency is vendored under third_party.
# No network access, no code generation, no download step.

VERSION      := $(shell cat VERSION)
SONAME_MAJOR := 0

PREFIX      ?= /usr/local
LIBDIR      ?= $(PREFIX)/lib
INCLUDEDIR  ?= $(PREFIX)/include
BINDIR      ?= $(PREFIX)/bin
PKGCONFIGDIR?= $(LIBDIR)/pkgconfig
DESTDIR     ?=

BUILD       ?= build

CC          ?= cc
CXX         ?= c++
AR          ?= ar

WARN_C   := -Wall -Wextra -Werror -Wshadow -Wconversion -Wsign-conversion \
            -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes \
            -Wformat=2 -Wvla
WARN_CXX := -Wall -Wextra

COMMON_FLAGS := -O2 -g -fPIC -fno-common -fstack-protector-strong \
                -D_FORTIFY_SOURCE=2 -DKAL_VERSION_STRING='"$(VERSION)"'
CFLAGS_KAL   := -std=c11 -D_GNU_SOURCE $(WARN_C) $(COMMON_FLAGS) \
                -Iinclude -Isrc -Ithird_party/ggwave/include
CXXFLAGS_KAL := -std=c++11 $(WARN_CXX) $(COMMON_FLAGS) \
                -fvisibility=hidden -Ithird_party/ggwave/include \
                -Ithird_party/ggwave/src
LDFLAGS_KAL  := -Wl,-z,relro -Wl,-z,now

SANITIZE := -fsanitize=address,undefined -fno-omit-frame-pointer \
            -fno-sanitize-recover=all

CORE_SRC := src/kal_frame.c src/kal_sha256.c src/kal_random.c \
            src/kal_modem.c src/kal_link.c
ENGINE_SRC := third_party/ggwave/src/ggwave.cpp
TOOL_SRC := tools/kal_cli.c tools/kal_audio.c

CORE_OBJ   := $(CORE_SRC:%.c=$(BUILD)/%.o)
ENGINE_OBJ := $(BUILD)/third_party/ggwave/src/ggwave.o
TOOL_OBJ   := $(TOOL_SRC:%.c=$(BUILD)/%.o)

STATIC_LIB := $(BUILD)/libkilix-acoustic-link.a
SHARED_LIB := $(BUILD)/libkilix-acoustic-link.so.$(VERSION)
SONAME     := libkilix-acoustic-link.so.$(SONAME_MAJOR)
TOOL       := $(BUILD)/kilix-acoustic-link
PC_FILE    := $(BUILD)/kilix-acoustic-link.pc

TEST_SRC := tests/test_frame.c tests/test_link.c tests/test_bounds.c \
            tests/test_modem.c tests/test_adapter.c
TEST_BIN := $(TEST_SRC:tests/%.c=$(BUILD)/%)
SAN_BIN  := $(TEST_SRC:tests/%.c=$(BUILD)/san-%) $(BUILD)/san-fuzz_frame
FUZZ_BIN := $(BUILD)/fuzz_frame

.PHONY: all clean test test-normal test-sanitize vectors install uninstall \
        check-offline help

all: $(STATIC_LIB) $(SHARED_LIB) $(TOOL) $(PC_FILE)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KAL) -c $< -o $@

$(ENGINE_OBJ): $(ENGINE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_KAL) -c $< -o $@

$(STATIC_LIB): $(CORE_OBJ) $(ENGINE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(SHARED_LIB): $(CORE_OBJ) $(ENGINE_OBJ)
	@mkdir -p $(dir $@)
	$(CXX) -shared -Wl,-soname,$(SONAME) $(LDFLAGS_KAL) $^ -o $@
	ln -sf $(notdir $(SHARED_LIB)) $(BUILD)/$(SONAME)
	ln -sf $(SONAME) $(BUILD)/libkilix-acoustic-link.so

$(TOOL): $(TOOL_OBJ) $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CXX) $(LDFLAGS_KAL) $(TOOL_OBJ) $(STATIC_LIB) -o $@

$(PC_FILE): kilix-acoustic-link.pc.in VERSION
	@mkdir -p $(dir $@)
	sed -e 's|@PREFIX@|$(PREFIX)|g' -e 's|@LIBDIR@|$(LIBDIR)|g' \
	    -e 's|@INCLUDEDIR@|$(INCLUDEDIR)|g' -e 's|@VERSION@|$(VERSION)|g' \
	    $< > $@

# Tests link the core sources directly with the test modem enabled, so the
# deterministic backend can never reach a release library.
$(BUILD)/%: tests/%.c $(CORE_SRC) tools/kal_audio.c $(ENGINE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KAL) -Itools -DKAL_ENABLE_TEST_MODEM=1 -Itests $< \
	    $(CORE_SRC) tools/kal_audio.c $(ENGINE_OBJ) -lstdc++ -lm -o $@

$(BUILD)/san-%: tests/%.c $(CORE_SRC) tools/kal_audio.c $(ENGINE_SRC)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS_KAL) $(SANITIZE) -c $(ENGINE_SRC) -o $(BUILD)/san-ggwave.o
	$(CC) $(CFLAGS_KAL) $(SANITIZE) -Itools -DKAL_ENABLE_TEST_MODEM=1 -Itests $< \
	    $(CORE_SRC) tools/kal_audio.c $(BUILD)/san-ggwave.o -lstdc++ -lm -o $@

$(FUZZ_BIN): tests/fuzz_frame.c $(CORE_SRC) $(ENGINE_OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KAL) -DKAL_ENABLE_TEST_MODEM=1 -Itests $< $(CORE_SRC) \
	    $(ENGINE_OBJ) -lstdc++ -lm -o $@

# The release library must refuse the deterministic test backend. This guard
# is built WITHOUT KAL_ENABLE_TEST_MODEM and links the shipped archive.
$(BUILD)/test_release_guard: tests/test_release_guard.c $(STATIC_LIB)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_KAL) -Itests $< $(STATIC_LIB) -lstdc++ -lm -o $@

test: test-normal test-sanitize

test-normal: $(TEST_BIN) $(FUZZ_BIN) $(BUILD)/test_release_guard
	@set -e; for binary in $(TEST_BIN); do echo "== $$binary"; $$binary; done
	@echo "== $(BUILD)/test_release_guard"; $(BUILD)/test_release_guard
	@echo "== $(FUZZ_BIN)"; $(FUZZ_BIN) tests/vectors/fuzz-corpus

test-sanitize: $(SAN_BIN)
	@set -e; for binary in $(SAN_BIN); do \
	    echo "== $$binary"; \
	    case $$binary in *fuzz_frame) $$binary tests/vectors/fuzz-corpus;; \
	                     *) $$binary;; esac; \
	done

vectors: $(BUILD)/test_frame
	$(BUILD)/test_frame --emit-vectors > tests/vectors/kal1-golden.txt

# The build and the tests must not touch the network. This target fails if
# any source names a fetch step.
check-offline:
	@! grep -rInE 'https?://[^ )"]*' --include='Makefile' --include='*.mk' . \
	  || (echo "network reference in build files" >&2; exit 1)
	@echo "offline build check: 1/1 pass"

install: all
	install -d $(DESTDIR)$(LIBDIR) $(DESTDIR)$(INCLUDEDIR) \
	           $(DESTDIR)$(BINDIR) $(DESTDIR)$(PKGCONFIGDIR)
	install -m 0644 $(STATIC_LIB) $(DESTDIR)$(LIBDIR)/
	install -m 0755 $(SHARED_LIB) $(DESTDIR)$(LIBDIR)/
	ln -sf $(notdir $(SHARED_LIB)) $(DESTDIR)$(LIBDIR)/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(LIBDIR)/libkilix-acoustic-link.so
	install -m 0644 include/kilix_acoustic_link.h $(DESTDIR)$(INCLUDEDIR)/
	install -m 0755 $(TOOL) $(DESTDIR)$(BINDIR)/
	install -m 0644 $(PC_FILE) $(DESTDIR)$(PKGCONFIGDIR)/

uninstall:
	rm -f $(DESTDIR)$(LIBDIR)/libkilix-acoustic-link.a
	rm -f $(DESTDIR)$(LIBDIR)/libkilix-acoustic-link.so*
	rm -f $(DESTDIR)$(INCLUDEDIR)/kilix_acoustic_link.h
	rm -f $(DESTDIR)$(BINDIR)/kilix-acoustic-link
	rm -f $(DESTDIR)$(PKGCONFIGDIR)/kilix-acoustic-link.pc

clean:
	rm -rf $(BUILD)

help:
	@echo "targets: all test test-normal test-sanitize vectors install uninstall clean check-offline"
