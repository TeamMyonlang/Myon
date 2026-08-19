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
# Step 5 unit tests for the AST -> MVM bytecode compiler (docs/mvm_spec.md).
#
# This is deliberately separate from tests/run_tests.sh (the .myon/.out
# tree-walk regression suite) because its goal is different: it verifies the
# *compiler* mechanically, not program output.  It checks that
#
#   1. every tests/cases/*.myon file either compiles+disassembles without
#      crashing, or fails with a clean (non-signal) compile error — never a
#      segfault/abort;
#   2. the MVM-out-of-scope features (async / net / http / ffi / generics,
#      spec §7) are rejected with the "MVM does not support ..." diagnostic and
#      no .myc is produced;
#   3. the hand-written tests/mvm_dump_samples/*.myon compile, disassemble, and
#      round-trip through a written .myc file (compile -> read back -> dump).

set -u
cd "$(dirname "$0")/.."

MYON=./myon
if [ ! -x "$MYON" ]; then
    echo "error: build the interpreter first (make)"; exit 1
fi

pass=0
fail=0
TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT

ok()   { echo "  ok   $1"; pass=$((pass + 1)); }
bad()  { echo "  FAIL $1"; fail=$((fail + 1)); }

# A process is considered to have "crashed" if it was killed by a signal, i.e.
# exit status >= 128 (bash reports 128 + signal number).
crashed() { [ "$1" -ge 128 ]; }

# ------------------------------------------------------------------ #
# 1. No tree-walk test case may crash the compiler.                  #
# ------------------------------------------------------------------ #
echo "== MVM compiler: --dump-bytecode must never crash on tests/cases =="
for t in tests/cases/*.myon; do
    name="$(basename "${t%.myon}")"
    "$MYON" --dump-bytecode "$t" >/dev/null 2>&1
    rc=$?
    if crashed "$rc"; then
        bad "$name (crashed with signal, exit=$rc)"
    else
        ok "$name (exit=$rc, no crash)"
    fi
done

# ------------------------------------------------------------------ #
# 2. Features that used to be MVM-out-of-scope now COMPILE cleanly.  #
#    async/await, net, ffi and generics were brought into the MVM    #
#    (they run via the tree-walk bridge / event loop), so the        #
#    compiler must accept them and emit a valid .myc.  We assert a    #
#    zero exit, no crash, and a real "MYC1" artifact.                 #
# ------------------------------------------------------------------ #
echo "== MVM compiler: async/net/ffi/generics now compile cleanly =="

# expect_compiles <label> <inline-myon-source>
expect_compiles() {
    local label="$1" src="$2"
    local f="$TMPD/$label.myon"
    local out="$TMPD/$label.myc"
    printf '%s\n' "$src" > "$f"
    local msg
    msg="$("$MYON" --compile "$f" -o "$out" 2>&1)"
    local rc=$?
    if crashed "$rc"; then
        bad "$label (crashed, exit=$rc)"
    elif [ "$rc" -ne 0 ]; then
        bad "$label (expected a clean compile, but exit was $rc: $msg)"
    elif [ ! -f "$out" ]; then
        bad "$label (no .myc produced)"
    elif [ "$(head -c 4 "$out")" != "MYC1" ]; then
        bad "$label (bad .myc magic)"
    else
        ok "$label (compiled to .myc)"
    fi
}

expect_compiles async   'myon.async myon.func f() ret void { }'
expect_compiles net     'module myon.net
s, e = myon.net.tcp_socket()'
expect_compiles ffi     'module myon.ffi
h, e = myon.ffi.load("libc.so.6")'
expect_compiles generic 'myon.func id<T>(x: T) ret T { ret x }'

# ------------------------------------------------------------------ #
# 3. Hand-written samples compile, disassemble and round-trip.       #
# ------------------------------------------------------------------ #
echo "== MVM compiler: dump samples compile + .myc round-trip =="
for t in tests/mvm_dump_samples/*.myon; do
    [ -e "$t" ] || continue
    name="$(basename "${t%.myon}")"

    # 3a. direct disassembly must succeed
    "$MYON" --dump-bytecode "$t" >/dev/null 2>&1
    if [ $? -ne 0 ]; then bad "$name (dump failed)"; continue; fi

    # 3b. compile to a .myc, then disassemble the .myc back
    myc="$TMPD/$name.myc"
    "$MYON" --compile "$t" -o "$myc" >/dev/null 2>&1
    if [ $? -ne 0 ] || [ ! -f "$myc" ]; then bad "$name (compile to .myc failed)"; continue; fi

    # magic bytes must be "MYC1"
    magic="$(head -c 4 "$myc")"
    if [ "$magic" != "MYC1" ]; then bad "$name (bad .myc magic: '$magic')"; continue; fi

    d_src="$("$MYON" --dump-bytecode "$t"   2>/dev/null | grep -v '^; ')"
    d_myc="$("$MYON" --dump-bytecode "$myc" 2>/dev/null | grep -v '^; ')"
    if [ "$d_src" != "$d_myc" ]; then
        bad "$name (.myc round-trip disassembly differs from source)"
        diff <(printf '%s' "$d_src") <(printf '%s' "$d_myc") | sed 's/^/      /'
    else
        ok "$name (compiled, magic ok, round-trip stable)"
    fi
done

echo "== results: $pass passed, $fail failed =="
[ "$fail" -eq 0 ]
