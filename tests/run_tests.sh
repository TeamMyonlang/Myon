#!/usr/bin/env bash
#
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
#
# Regression tests for Myon Steps 0-5.

set -u
cd "$(dirname "$0")/.."

MYON=./myon
if [ ! -x "$MYON" ]; then
    echo "error: build the interpreter first (make)"; exit 1
fi

pass=0
fail=0
skip=0

# ---------------------------------------------------------------------------
# Platform detection for OS-specific test cases.
#
# A handful of FFI cases hard-code Linux shared-object *file names* in the
# .myon source itself (libm.so.6, libz.so.1, tests/fixtures/*.so) and their
# golden .out files bake in the Linux result.  Those names do not exist on
# macOS (libm.dylib / libz.dylib, inside libSystem) or Windows (msvcrt.dll /
# zlib1.dll), so the exact-output comparison would spuriously fail there even
# though the FFI subsystem itself is working.  Rather than fork the golden
# outputs per OS, such cases are SKIPPED (not failed) off Linux and reported
# as "skip".  Everything else -- including the genuinely cross-platform
# net/async/http loopback cases -- still runs everywhere, which is what
# actually exercises the macOS/Windows platform split (arc4random, SO_NOSIGPIPE,
# ucontext, Winsock, dlopen/LoadLibrary).
# ---------------------------------------------------------------------------
UNAME_S="$(uname -s 2>/dev/null || echo unknown)"
case "$UNAME_S" in
    Linux) IS_LINUX=1 ;;
    *)     IS_LINUX=0 ;;
esac

# Cases whose .myon source hard-codes a Linux ".so" library name via
# myon.ffi.load() (and whose golden .out therefore only matches on Linux):
#
#   p_ffi_basic / p_ffi_close        load "libm.so.6"
#   p31_ffi_bytes_crc32 / _read_cstr load "libz.so.1"
#   p41_ffi_callback                 loads the Linux-only .so fixture built above
#
# These are skipped on non-Linux hosts.  NOTE: the pure-memory FFI cases
# (alloc/free/read/write_array/struct_dsl) are deliberately NOT in this list --
# they load no external library, so they are cross-platform and DO run on
# macOS/Windows, proving the FFI subsystem itself works there.  Likewise
# p3_ffi_load_missing / p_ffi_load_fail exercise the *failure* path with
# normalized output and run everywhere.
LINUX_ONLY_SO_CASES=" \
p_ffi_basic p_ffi_close p31_ffi_bytes_crc32 p31_ffi_read_cstr \
p41_ffi_callback "

# is_linux_only_so_case <name> -> 0 (true) if it must be skipped off Linux.
is_linux_only_so_case() {
    case " $LINUX_ONLY_SO_CASES " in (*" $1 "*) return 0 ;; esac
    return 1
}

# Phase4.1: build the FFI-callback fixture shared library the callback test
# depends on.  Best-effort — if the C compiler or dlopen is unavailable the
# p41_ffi_callback case simply prints "load_fail" and its .out reflects that.
# Only attempted on Linux, since the .so-name FFI cases are skipped elsewhere.
CB_SRC="tests/fixtures/ffi_callback_test.c"
CB_SO="tests/fixtures/libffi_callback_test.so"
if [ "$IS_LINUX" -eq 1 ] && [ -f "$CB_SRC" ]; then
    CC_BIN="${CC:-cc}"
    "$CC_BIN" -shared -fPIC -o "$CB_SO" "$CB_SRC" 2>/dev/null \
        && echo "  (built $CB_SO)" \
        || echo "  (warning: could not build $CB_SO; callback test may skip)"
fi

# Strip carriage returns so golden comparisons are line-ending agnostic.
# The interpreter itself now forces LF output on Windows (main.c sets stdout to
# binary mode), but the .out fixtures are committed with LF and a checkout under
# a CRLF-normalizing Git config (or a stray tool in the pipeline) could still
# reintroduce '\r'.  Normalizing both sides here makes the suite robust to that
# on MSYS2/MINGW without weakening the Linux/macOS comparison (which have no CR).
strip_cr() { tr -d '\r'; }

# check_output <name> <myon-file> <expected-file>
check_output() {
    local name="$1" src="$2" expected="$3"
    local got expected_txt
    got=$("$MYON" "$src" 2>/dev/null | strip_cr)
    expected_txt=$(strip_cr < "$expected")
    if [ "$got" == "$expected_txt" ]; then
        echo "  ok   $name"
        pass=$((pass + 1))
    else
        echo "  FAIL $name"
        echo "    --- expected ---"; printf '%s\n' "$expected_txt" | sed 's/^/    /'
        echo "    --- got ---";      printf '%s\n' "$got" | sed 's/^/    /'
        fail=$((fail + 1))
    fi
}

# check_error <name> <myon-file>  (expects a non-zero exit)
check_error() {
    local name="$1" src="$2"
    "$MYON" "$src" >/dev/null 2>&1
    if [ $? -ne 0 ]; then
        echo "  ok   $name (errored as expected)"
        pass=$((pass + 1))
    else
        echo "  FAIL $name (expected an error, but exit was 0)"
        fail=$((fail + 1))
    fi
}

echo "== Myon Steps 0-5 tests =="
for t in tests/cases/*.myon; do
    base="${t%.myon}"
    name="$(basename "$base")"
    # Off Linux, skip the FFI cases that hard-code Linux ".so" library names
    # (see LINUX_ONLY_SO_CASES above) so they don't fail on the golden-output
    # comparison for a reason unrelated to the platform code under test.
    if [ "$IS_LINUX" -eq 0 ] && is_linux_only_so_case "$name"; then
        echo "  skip $name (Linux-only .so name; not applicable on $UNAME_S)"
        skip=$((skip + 1))
        continue
    fi
    if [ -f "$base.out" ]; then
        check_output "$name" "$t" "$base.out"
    elif [ -f "$base.err" ]; then
        check_error "$name" "$t"
    else
        echo "  ??   $name (no .out or .err fixture)"
    fi
done

# CLI smoke test: --version / -v must print "myon <X.Y.Z>" and exit 0.
check_version_flag() {
    local flag="$1"
    local got rc
    got=$("$MYON" "$flag" 2>/dev/null); rc=$?
    if [ "$rc" -eq 0 ] && [[ "$got" =~ ^myon\ [0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "  ok   cli_version ($flag -> '$got')"
        pass=$((pass + 1))
    else
        echo "  FAIL cli_version ($flag: exit $rc, got '$got')"
        fail=$((fail + 1))
    fi
}
check_version_flag --version
check_version_flag -v

if [ "$skip" -gt 0 ]; then
    echo "== results: $pass passed, $fail failed, $skip skipped =="
else
    echo "== results: $pass passed, $fail failed =="
fi
[ "$fail" -eq 0 ]
