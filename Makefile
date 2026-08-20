# Copyright 2026 TeamMyonlang
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Wpedantic -O2
# Phase5.1 Step6: HTTPS/TLS in myon.http is implemented natively against
# OpenSSL, so the OpenSSL development package (libssl-dev / openssl-devel) is
# a required build dependency and we always link libssl/libcrypto.
LDLIBS  ?= -lm -lssl -lcrypto

# Phase3 C FFI needs the dynamic loader (dlopen/dlsym/dlclose) on Linux.
# On macOS these live in libSystem (no extra flag); Windows uses its own API.
UNAME_S := $(shell uname -s 2>/dev/null)

SRC_DIR  = src
BUILD    = build

# Platform branch.
#
# Windows (native MSYS2/MinGW-w64, or a MinGW-w64 cross build via
# CC=x86_64-w64-mingw32-gcc) is detected through the OS environment variable,
# which MSYS2/MinGW shells and Windows both set to "Windows_NT".  The Linux
# branch below is left exactly as before so the existing Linux build is not
# affected in any way.
ifeq ($(OS),Windows_NT)
# Windows build.
#   * The FFI layer calls LoadLibrary/GetProcAddress/FreeLibrary, which live in
#     kernel32.dll and are linked implicitly by the toolchain, so no -ldl (and
#     no other extra loader flag) is needed here.
#   * The output binary is named myon.exe.
BIN      = myon.exe
# The myon.net socket layer (src/net.c, _WIN32 branch) is implemented against
# Winsock2, so the Windows link line must pull in the Winsock 2 library
# (ws2_32.dll -> -lws2_32).  This has no effect on the Linux branch below.
LDLIBS  += -lws2_32
# NOTE (OpenSSL on Windows): myon.http links libssl/libcrypto (see LDLIBS
# above).  The default -lssl -lcrypto names work with the MSYS2
# `mingw-w64-x86_64-openssl` package and with vcpkg's OpenSSL, provided the
# corresponding include/ and lib/ (import libraries libssl.dll.a /
# libcrypto.dll.a) are on the compiler's search path.  A MinGW-w64 cross build
# (CC=x86_64-w64-mingw32-gcc) links cleanly this way -- verified in Step 7-c.
#
# If a particular toolchain uses different names/paths (e.g.
# -lssl-3-x64 / -lcrypto-3-x64, or a non-default prefix), add them via
# WIN_OPENSSL_LDLIBS without touching the Linux defaults, e.g.:
#     make WIN_OPENSSL_LDLIBS="-L/mingw64/lib"
# WIN_OPENSSL_LDLIBS folds into LDLIBS only on Windows, so the Linux link line
# is unchanged.
WIN_OPENSSL_LDLIBS ?=
LDLIBS  += $(WIN_OPENSSL_LDLIBS)
else
BIN      = myon
ifeq ($(UNAME_S),Linux)
# Linux: the C FFI dynamic loader (dlopen/dlsym/dlclose) lives in libdl.
LDLIBS  += -ldl
endif
ifeq ($(UNAME_S),Darwin)
# macOS build.
#   * dlopen/dlsym/dlclose live in libSystem, so NO -ldl is needed (adding it
#     would fail: there is no standalone libdl on macOS).
#   * Homebrew keeps OpenSSL "keg-only" (not on the default compiler search
#     path).  Rather than hard-code a prefix, ask brew where it is and fold the
#     include/lib paths in automatically when brew + openssl are present.  This
#     works for both Apple Silicon (/opt/homebrew) and Intel (/usr/local)
#     without the caller having to set anything.  If brew/openssl is absent the
#     variables are simply empty and the default -lssl/-lcrypto search is used
#     (e.g. a MacPorts / system OpenSSL already on the path).
BREW_OPENSSL := $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(BREW_OPENSSL),)
CFLAGS  += -I$(BREW_OPENSSL)/include
LDLIBS  += -L$(BREW_OPENSSL)/lib
endif
endif
endif

SOURCES  = $(wildcard $(SRC_DIR)/*.c)
OBJECTS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SOURCES))

# Installation paths.  Override PREFIX (or DESTDIR for staged installs) as
# needed, e.g.  `make install PREFIX=$HOME/.local`  or
# `make install DESTDIR=/tmp/pkg`.  A system-wide `make install` typically
# needs root (e.g. `sudo make install`).
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DESTDIR ?=
INSTALL ?= install

.PHONY: all clean test test-mvm test-mvm-equality test-pkg win-cross asan test-asan install uninstall

all: $(BIN)

# Convenience: MinGW-w64 cross-compile from Linux, producing myon.exe.
# Requires the x86_64-w64-mingw32 toolchain and Windows OpenSSL headers/import
# libraries (MSYS2 mingw-w64-x86_64-openssl or vcpkg) on its search path.
# This only cross-links; it does NOT run the resulting .exe (no Windows here).
# Extra OpenSSL search paths, if needed, go via WIN_OPENSSL_LDLIBS, e.g.:
#     make win-cross WIN_OPENSSL_LDLIBS="-L/path/to/openssl/lib"
win-cross:
	$(MAKE) OS=Windows_NT CC=x86_64-w64-mingw32-gcc

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDLIBS)

# ------------------------------------------------------------------------- #
# Sanitizer build (AddressSanitizer + UndefinedBehaviorSanitizer).
#
# This mirrors the ASan/UBSan build documented in flaw-and-Add.md
# ("検証方法（再現手順）") that was used to find A-1..A-6.  It is a single
# self-contained link of all sources (no build/ objects reuse) at -O0 -g so
# stack traces stay readable, and it always produces a binary named
# `myon_asan` so it never clobbers the real `myon` binary.
#
# NOTE on link libraries: the sanitizer build is native-only (it is meant for
# the sanitizer CI job, which runs on Linux/macOS -- never Windows).  It uses an
# explicit POSIX link line rather than $(LDLIBS) so a stray OS=Windows_NT or a
# WIN_OPENSSL_LDLIBS override in the environment cannot leak in.  -ldl is added
# only on Linux (macOS has dlopen in libSystem and has no standalone libdl, so
# linking -ldl there would fail); OpenSSL include/lib paths from Homebrew are
# folded in on macOS via the same brew --prefix probe as the normal build.
ASAN_BIN    = myon_asan
ASAN_CC    ?= $(CC)
ASAN_CFLAGS = -std=c11 -g -O0 -fsanitize=address,undefined \
              -fno-omit-frame-pointer
ASAN_LDLIBS = -lm -lssl -lcrypto
ifeq ($(UNAME_S),Linux)
ASAN_LDLIBS += -ldl
endif
ifeq ($(UNAME_S),Darwin)
ASAN_BREW_OPENSSL := $(shell brew --prefix openssl@3 2>/dev/null)
ifneq ($(ASAN_BREW_OPENSSL),)
ASAN_CFLAGS += -I$(ASAN_BREW_OPENSSL)/include
ASAN_LDLIBS += -L$(ASAN_BREW_OPENSSL)/lib
endif
endif

asan: $(ASAN_BIN)

$(ASAN_BIN): $(SOURCES)
	$(ASAN_CC) $(ASAN_CFLAGS) -I$(SRC_DIR) $(SOURCES) -o $@ $(ASAN_LDLIBS)

# Run the full regression suite against the sanitizer binary.
#
# The four test scripts (run_tests.sh / mvm_compiler_tests.sh /
# run_mvm_tests.sh / bench_mvm.sh) hardcode `MYON=./myon`, so rather than
# patch all of them we temporarily swap the sanitizer binary in as `./myon`,
# run the suite, then restore the original.  The swap is done with a trap so
# the original `myon` is always put back even if a test fails (approach B in
# the CI TODO).  ASAN_OPTIONS/UBSAN_OPTIONS make any sanitizer finding abort
# with a non-zero exit so the failure surfaces as a red CI step.
test-asan: $(ASAN_BIN)
	@echo "== Myon sanitizer (ASan/UBSan) test run =="
	@set -e; \
	restore() { \
	  if [ -f myon.asan-backup ]; then mv -f myon.asan-backup myon; \
	  else rm -f myon; fi; \
	  rm -f myon.asan-backup; \
	}; \
	trap restore EXIT INT TERM; \
	if [ -e myon ]; then cp -f myon myon.asan-backup; fi; \
	cp -f $(ASAN_BIN) myon; \
	export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0"; \
	export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1:abort_on_error=1"; \
	./tests/run_tests.sh; \
	./tests/mvm_compiler_tests.sh; \
	if [ -f ./tests/run_mvm_tests.sh ]; then ./tests/run_mvm_tests.sh; fi

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

# Full test suite.  The existing suites (run order and output format) are kept
# exactly as before; Step 7-c only appends the .myon/.myc equality suite when
# it is present (tests/run_mvm_tests.sh), so older checkouts still work.
test: all test-pkg
	./tests/run_tests.sh
	./tests/mvm_compiler_tests.sh
	@if [ -f ./tests/run_mvm_tests.sh ]; then ./tests/run_mvm_tests.sh; fi

# ------------------------------------------------------------------------- #
# Package-manager C unit / integration tests (spec §11).
#
# These are standalone C test programs (not .myon scripts), so they are built
# and run directly here rather than through the .myon test-runner scripts.
# All three link the whole package-manager translation-unit set plus the
# net/tls layer that the fetch layer's default transport references; the
# resolver/install pipeline is exercised entirely offline through an injected
# mock transport (spec §11.2), so `make test-pkg` needs no network.
#
#   pkg_unit_tests  - manifest / package.myon / lockfile parsers + validators
#   pkg_zip_tests   - security-first ZIP reader + SHA-256 known-answer tests
#   pkg_ops_tests   - resolver + `pkg lock` / `install` / `install <url>`
#
# A POSIX feature-test macro is defined because the tests (and pkg_fs.c) use
# strdup / getcwd / mkdir etc.  The tests are compiled with the same strict
# warning flags as the interpreter.
PKG_TEST_SRCS = $(SRC_DIR)/package.c $(SRC_DIR)/common.c \
                $(SRC_DIR)/pkg_hash.c $(SRC_DIR)/pkg_fs.c $(SRC_DIR)/pkg_zip.c \
                $(SRC_DIR)/pkg_fetch.c $(SRC_DIR)/pkg_ops.c \
                $(SRC_DIR)/net.c $(SRC_DIR)/tls.c
PKG_TEST_CFLAGS = $(CFLAGS) -D_POSIX_C_SOURCE=200809L -I$(SRC_DIR)
PKG_TEST_LDLIBS = -lssl -lcrypto
ifeq ($(UNAME_S),Linux)
PKG_TEST_LDLIBS += -ldl
endif

test-pkg: | $(BUILD)
	@echo "== Myon package-manager C tests =="
	$(CC) $(PKG_TEST_CFLAGS) tests/pkg_unit_tests.c $(PKG_TEST_SRCS) -o $(BUILD)/pkg_unit_tests $(PKG_TEST_LDLIBS)
	$(CC) $(PKG_TEST_CFLAGS) tests/pkg_zip_tests.c  $(PKG_TEST_SRCS) -o $(BUILD)/pkg_zip_tests  $(PKG_TEST_LDLIBS)
	$(CC) $(PKG_TEST_CFLAGS) tests/pkg_ops_tests.c  $(PKG_TEST_SRCS) -o $(BUILD)/pkg_ops_tests  $(PKG_TEST_LDLIBS)
	$(BUILD)/pkg_unit_tests
	$(BUILD)/pkg_zip_tests
	$(BUILD)/pkg_ops_tests

# Step 5: run only the AST -> MVM bytecode compiler unit tests.
test-mvm: all
	./tests/mvm_compiler_tests.sh

# Step 7-b: run only the .myon (tree-walk) vs .myc (MVM) equality suite.
test-mvm-equality: all
	./tests/run_mvm_tests.sh

# Install the built interpreter into $(DESTDIR)$(BINDIR) (default
# /usr/local/bin).  Depends on the binary so `make install` builds first.
install: $(BIN)
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -m 0755 $(BIN) $(DESTDIR)$(BINDIR)/$(BIN)
	@echo "installed $(BIN) -> $(DESTDIR)$(BINDIR)/$(BIN)"

# Remove a previously installed binary.
uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(BIN)
	@echo "removed $(DESTDIR)$(BINDIR)/$(BIN)"

clean:
	rm -rf $(BUILD) $(BIN) $(ASAN_BIN) myon.asan-backup
