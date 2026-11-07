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
# Step 7-b: mechanical .myon (tree-walk) vs .myc (MVM) equality suite.
#
# Run each fixture once per engine. Detect unsupported features from the VM
# result already captured for the comparison, without a preflight execution.

set -u
cd "$(dirname "$0")/.."

MYON=./myon
if [ ! -x "$MYON" ]; then
    echo "error: build the interpreter first (make)"; exit 1
fi

# Per-case wall-clock guard so a (hypothetical) VM infinite loop cannot hang
# the whole suite.
TIMEOUT="${MVM_TEST_TIMEOUT:-30}"
# Homebrew installs GNU timeout as gtimeout; never silently drop the guard.
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN=gtimeout
else
    echo "error: GNU timeout is required (coreutils; gtimeout on macOS)" >&2
    exit 1
fi
run_to() { "$TIMEOUT_BIN" "$TIMEOUT" "$@"; }

# Current limitations; actual exclusions still come from VM diagnostics.
EXCLUDED_KNOWN=(p41_ffi_callback p4_array_higher_order p_module_alias)

scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

echo "== MVM .myon/.myc equality suite (Step 7-b) =="
echo "   (documented exclusions: ${EXCLUDED_KNOWN[*]})"
echo

pass=0
fail=0
excluded=0
FAILED_NAMES=""
EXCLUDED_NAMES=""

for t in tests/cases/*.myon; do
    base="${t%.myon}"
    name="$(basename "$base")"

    if [ ! -f "$base.out" ] && [ ! -f "$base.err" ]; then
        echo "  FAIL $name (no .out or .err fixture)"
        fail=$((fail + 1))
        FAILED_NAMES="$FAILED_NAMES $name"
        continue
    fi

    run_to "$MYON" "$t" >"$scratch/tw.out" 2>"$scratch/tw.err"; tw_rc=$?
    run_to "$MYON" --run-mvm "$t" >"$scratch/vm.out" 2>"$scratch/vm.err"; vm_rc=$?
    if { [ "$tw_rc" -eq 0 ] || [ "$tw_rc" -eq 1 ] || [ "$tw_rc" -eq 65 ]; } &&
       { [ "$vm_rc" -eq 1 ] || [ "$vm_rc" -eq 65 ]; } &&
       grep -q 'MVM does not support' "$scratch/vm.err"; then
        echo "  excl $name (MVM out-of-scope feature, spec §7)"
        excluded=$((excluded + 1))
        EXCLUDED_NAMES="$EXCLUDED_NAMES $name"
        continue
    fi

    if [ -f "$base.out" ]; then
        # Output case: both engines must produce identical stdout.
        tw="$(cat "$scratch/tw.out")"
        vm="$(cat "$scratch/vm.out")"
        if [ "$tw_rc" -eq 0 ] && [ "$vm_rc" -eq 0 ] && [ "$tw" == "$vm" ]; then
            echo "  ok   $name (tree-walk == MVM)"
            pass=$((pass + 1))
        else
            echo "  FAIL $name (tree-walk=$tw_rc, MVM=$vm_rc; output mismatch or abnormal exit)"
            echo "    --- tree-walk (.myon) ---"; printf '%s\n' "$tw" | sed 's/^/    /'
            echo "    --- MVM (--run-mvm)   ---"; printf '%s\n' "$vm" | sed 's/^/    /'
            fail=$((fail + 1))
            FAILED_NAMES="$FAILED_NAMES $name"
        fi
    elif [ -f "$base.err" ]; then
        # Require the same error stage, unless a fixture pins both exit codes.
        expected_tw=$tw_rc
        expected_vm=$tw_rc
        if [ -f "$base.mvm-exit" ]; then
            expected=$(tr -d '\r' < "$base.mvm-exit")
            case "$expected" in
                '1 1'|'1 65'|'65 1'|'65 65')
                    read -r expected_tw expected_vm <<< "$expected" ;;
                *)
                    echo "  FAIL $name (invalid .mvm-exit; expected two codes: 1 or 65)"
                    fail=$((fail + 1))
                    FAILED_NAMES="$FAILED_NAMES $name"
                    continue ;;
            esac
        fi
        if { [ "$tw_rc" -eq 1 ] || [ "$tw_rc" -eq 65 ]; } &&
           [ "$tw_rc" -eq "$expected_tw" ] && [ "$vm_rc" -eq "$expected_vm" ]; then
            echo "  ok   $name (both errored: tree-walk=$tw_rc, MVM=$vm_rc)"
            pass=$((pass + 1))
        else
            echo "  FAIL $name (exit differs: tree-walk=$tw_rc, MVM=$vm_rc)"
            fail=$((fail + 1))
            FAILED_NAMES="$FAILED_NAMES $name"
        fi
    fi
done

echo
echo "== results: $pass passed, $fail failed, $excluded excluded =="
[ -n "$FAILED_NAMES" ]   && echo "   failed:  $FAILED_NAMES"
[ -n "$EXCLUDED_NAMES" ] && echo "   excluded:$EXCLUDED_NAMES"
[ "$fail" -eq 0 ]
