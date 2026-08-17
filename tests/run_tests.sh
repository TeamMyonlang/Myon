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

# Phase4.1: build the FFI-callback fixture shared library the callback test
# depends on.  Best-effort — if the C compiler or dlopen is unavailable the
# p41_ffi_callback case simply prints "load_fail" and its .out reflects that.
CB_SRC="tests/fixtures/ffi_callback_test.c"
CB_SO="tests/fixtures/libffi_callback_test.so"
if [ -f "$CB_SRC" ]; then
    CC_BIN="${CC:-cc}"
    "$CC_BIN" -shared -fPIC -o "$CB_SO" "$CB_SRC" 2>/dev/null \
        && echo "  (built $CB_SO)" \
        || echo "  (warning: could not build $CB_SO; callback test may skip)"
fi

# check_output <name> <myon-file> <expected-file>
check_output() {
    local name="$1" src="$2" expected="$3"
    local got
    got=$("$MYON" "$src" 2>/dev/null)
    if [ "$got" == "$(cat "$expected")" ]; then
        echo "  ok   $name"
        pass=$((pass + 1))
    else
        echo "  FAIL $name"
        echo "    --- expected ---"; sed 's/^/    /' "$expected"
        echo "    --- got ---";      echo "$got" | sed 's/^/    /'
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
    if [ -f "$base.out" ]; then
        check_output "$name" "$t" "$base.out"
    elif [ -f "$base.err" ]; then
        check_error "$name" "$t"
    else
        echo "  ??   $name (no .out or .err fixture)"
    fi
done

echo "== results: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
