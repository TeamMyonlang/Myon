#!/usr/bin/env bash
#
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
#
# Step 7-b: mechanical .myon (tree-walk) vs .myc (MVM) equality suite.
#
# For every tests/cases/*.myon that is in the MVM-supported feature set
# (Step 4: no async / net / http / ffi / generics), this script runs it BOTH
# through the tree-walking interpreter (`myon file.myon`) and through the MVM
# bytecode VM (`myon --run-mvm file.myon`, i.e. compile-in-memory + run with
# the struct/method program available) and asserts the two engines produce the
# *identical* stdout (for .out cases) or both fail (for .err cases).
#
# The `--run-mvm` path is the apples-to-apples counterpart of tree-walk
# execution: it keeps the parsed Program available to the VM so struct-using
# programs behave identically.  (Running a serialized `.myc` off disk instead
# drops struct declarations by design -- see mvm_vm.h -- so it is NOT used for
# this equality check.)
#
# Cases that use MVM-out-of-scope features are detected dynamically (the
# compiler rejects them with a "MVM does not support ..." diagnostic) and are
# listed as EXCLUDED rather than counted as failures.  The known excluded set
# is also written out explicitly below for documentation.
#
# Step 7-c will wire this into `make test`; for now run it directly:
#     bash tests/run_mvm_tests.sh

set -u
cd "$(dirname "$0")/.."

MYON=./myon
if [ ! -x "$MYON" ]; then
    echo "error: build the interpreter first (make)"; exit 1
fi

# Per-case wall-clock guard so a (hypothetical) VM infinite loop cannot hang
# the whole suite.
TIMEOUT="${MVM_TEST_TIMEOUT:-30}"
run_to() { timeout "$TIMEOUT" "$@"; }

# ------------------------------------------------------------------ #
# Documented exclusion list (MVM out-of-scope features, spec §7).    #
# These are the async/net/http/ffi/generics cases; the loop below    #
# still re-detects exclusions dynamically from the compiler, so this #
# list is documentation, not the source of truth.                    #
# ------------------------------------------------------------------ #
EXCLUDED_KNOWN=(
    # C FFI (module myon.ffi)
    p_ffi_basic p_ffi_close p_ffi_load_fail p3_ffi_load_missing
    p31_ffi_alloc_free p31_ffi_bytes_crc32 p31_ffi_read_cstr p31_ffi_read_i64
    p41_ffi_array_bulk p41_ffi_callback p41_ffi_struct_dsl p41_ffi_write_typed
    # async / await
    p5_async_order p5_async_wait_task step17_async
    # net / http
    p5_net_tcp_echo p5_net_udp_echo p5_http_serve_static p51_net_dns_localhost
    # generics
    step15_generics
    # higher-order native methods driven by a VM lambda (array.map/filter/
    # reduce): a VM closure cannot be executed by the tree-walk built-in across
    # the engine boundary, so the VM refuses at run time (spec §7 limitation).
    p4_array_higher_order
)

echo "== MVM .myon/.myc equality suite (Step 7-b) =="
echo "   (documented exclusions: ${EXCLUDED_KNOWN[*]})"
echo

pass=0
fail=0
excluded=0
FAILED_NAMES=""
EXCLUDED_NAMES=""

# Return success if `src` uses an MVM-out-of-scope feature (spec §7).  This is
# detected honestly from the engine's own "MVM does not support ..."
# diagnostic, emitted either
#   (a) at COMPILE time  (e.g. async / net / http / ffi / generics), or
#   (b) at RUN time      (e.g. passing a VM lambda to a higher-order native
#                          method like array.map/filter/reduce -- the VM cannot
#                          drive a tree-walk callback across engine boundaries,
#                          so it refuses with a clear message instead of
#                          silently diverging or crashing).
# In both cases the feature is genuinely unsupported, so the case is EXCLUDED
# rather than counted as a mismatch (we never fake a "match").
is_unsupported() {
    local src="$1"
    local msg rc
    # (a) compile-time rejection
    msg="$("$MYON" --compile "$src" -o /dev/null 2>&1)"; rc=$?
    if [ "$rc" -ne 0 ] && printf '%s' "$msg" | grep -q "MVM does not support"; then
        return 0
    fi
    # (b) run-time rejection by the VM
    msg="$(run_to "$MYON" --run-mvm "$src" 2>&1)"
    if printf '%s' "$msg" | grep -q "MVM does not support"; then
        return 0
    fi
    return 1
}

for t in tests/cases/*.myon; do
    base="${t%.myon}"
    name="$(basename "$base")"

    # Dynamically skip MVM-out-of-scope features.
    if is_unsupported "$t"; then
        echo "  excl $name (MVM out-of-scope feature, spec §7)"
        excluded=$((excluded + 1))
        EXCLUDED_NAMES="$EXCLUDED_NAMES $name"
        continue
    fi

    if [ -f "$base.out" ]; then
        # Output case: both engines must produce identical stdout.
        tw="$(run_to "$MYON" "$t" 2>/dev/null)"
        vm="$(run_to "$MYON" --run-mvm "$t" 2>/dev/null)"
        if [ "$tw" == "$vm" ]; then
            echo "  ok   $name (tree-walk == MVM)"
            pass=$((pass + 1))
        else
            echo "  FAIL $name (tree-walk != MVM)"
            echo "    --- tree-walk (.myon) ---"; printf '%s\n' "$tw" | sed 's/^/    /'
            echo "    --- MVM (--run-mvm)   ---"; printf '%s\n' "$vm" | sed 's/^/    /'
            fail=$((fail + 1))
            FAILED_NAMES="$FAILED_NAMES $name"
        fi
    elif [ -f "$base.err" ]; then
        # Error case: both engines must fail (non-zero exit).
        run_to "$MYON" "$t" >/dev/null 2>&1;             tw_rc=$?
        run_to "$MYON" --run-mvm "$t" >/dev/null 2>&1;   vm_rc=$?
        if [ "$tw_rc" -ne 0 ] && [ "$vm_rc" -ne 0 ]; then
            echo "  ok   $name (both errored: tree-walk=$tw_rc, MVM=$vm_rc)"
            pass=$((pass + 1))
        else
            echo "  FAIL $name (exit differs: tree-walk=$tw_rc, MVM=$vm_rc)"
            fail=$((fail + 1))
            FAILED_NAMES="$FAILED_NAMES $name"
        fi
    else
        echo "  ??   $name (no .out or .err fixture)"
    fi
done

echo
echo "== results: $pass passed, $fail failed, $excluded excluded =="
[ -n "$FAILED_NAMES" ]   && echo "   failed:  $FAILED_NAMES"
[ -n "$EXCLUDED_NAMES" ] && echo "   excluded:$EXCLUDED_NAMES"
[ "$fail" -eq 0 ]
