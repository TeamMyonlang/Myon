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
# Step 7-b (optional): micro-benchmark comparing tree-walk (.myon) vs MVM
# bytecode (--run-mvm) execution time on a couple of CPU-bound workloads
# (recursive fibonacci + a tight counting loop).
#
# This is a rough wall-clock comparison, not a rigorous benchmark; it just
# gives a sense of the MVM's relative speed.  Both engines run the SAME source
# and (as run_mvm_tests.sh asserts) produce identical output, so any timing
# difference is purely the execution strategy.
#
#     bash tests/bench_mvm.sh            # default sizes
#     BENCH_FIB=32 BENCH_LOOP=2000000 bash tests/bench_mvm.sh

set -u
cd "$(dirname "$0")/.."

MYON=./myon
if [ ! -x "$MYON" ]; then
    echo "error: build the interpreter first (make)"; exit 1
fi

FIB_N="${BENCH_FIB:-30}"
LOOP_N="${BENCH_LOOP:-1000000}"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- workload 1: recursive fibonacci ------------------------------------ #
cat > "$TMP/fib.myon" <<EOF
module myon.stdio
myon.func fib(n: int) ret int {
    myon.if n < 2 then { ret n }
    ret fib(n - 1) + fib(n - 2)
}
myon.print(fib($FIB_N))
EOF

# --- workload 2: tight counting loop ------------------------------------ #
cat > "$TMP/loop.myon" <<EOF
module myon.stdio
i = 0
s = 0
myon.while i < $LOOP_N {
    s = s + i
    i = i + 1
}
myon.print(s)
EOF

# Portable millisecond timer around a command; the command's stdout is written
# to $TMP/last_out so the caller can read it (command substitution would run
# the timer in a subshell and lose a variable assignment under `set -u`).
time_ms() {
    local start end
    start=$(date +%s%N)
    "$@" >"$TMP/last_out" 2>/dev/null
    end=$(date +%s%N)
    echo $(( (end - start) / 1000000 ))
}

bench_one() {
    local label="$1"; shift
    local src="$1"; shift

    local tw_ms vm_ms tw_out vm_out
    tw_ms=$(time_ms "$MYON" "$src");            tw_out="$(cat "$TMP/last_out")"
    vm_ms=$(time_ms "$MYON" --run-mvm "$src");  vm_out="$(cat "$TMP/last_out")"

    local match="MATCH"
    [ "$tw_out" != "$vm_out" ] && match="DIFFER(!)"

    local ratio="n/a"
    if [ "$vm_ms" -gt 0 ]; then
        ratio="$(awk "BEGIN{printf \"%.2fx\", $tw_ms/$vm_ms}")"
    fi

    printf "  %-22s tree-walk=%6d ms   MVM=%6d ms   speedup=%-7s [%s]\n" \
        "$label" "$tw_ms" "$vm_ms" "$ratio" "$match"
}

echo "== MVM micro-benchmark (Step 7-b, optional) =="
echo "   fib(n)=$FIB_N   loop iterations=$LOOP_N"
echo "   (speedup = tree-walk time / MVM time; >1.00x means MVM is faster)"
echo
bench_one "fib($FIB_N)"        "$TMP/fib.myon"
bench_one "loop($LOOP_N)"      "$TMP/loop.myon"
echo
echo "note: rough wall-clock timing; both engines produce identical output."
