# Copyright 2026 nyan<(nyan4)
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
LDLIBS  += -ldl
endif
endif

SOURCES  = $(wildcard $(SRC_DIR)/*.c)
OBJECTS  = $(patsubst $(SRC_DIR)/%.c,$(BUILD)/%.o,$(SOURCES))

.PHONY: all clean test test-mvm test-mvm-equality win-cross

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

$(BUILD)/%.o: $(SRC_DIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD):
	mkdir -p $(BUILD)

# Full test suite.  The existing suites (run order and output format) are kept
# exactly as before; Step 7-c only appends the .myon/.myc equality suite when
# it is present (tests/run_mvm_tests.sh), so older checkouts still work.
test: all
	./tests/run_tests.sh
	./tests/mvm_compiler_tests.sh
	@if [ -f ./tests/run_mvm_tests.sh ]; then ./tests/run_mvm_tests.sh; fi

# Step 5: run only the AST -> MVM bytecode compiler unit tests.
test-mvm: all
	./tests/mvm_compiler_tests.sh

# Step 7-b: run only the .myon (tree-walk) vs .myc (MVM) equality suite.
test-mvm-equality: all
	./tests/run_mvm_tests.sh

clean:
	rm -rf $(BUILD) $(BIN)
