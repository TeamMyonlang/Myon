#!/usr/bin/env bash
# Check exact error statuses and diagnostics independently of .err fixtures.
set -eu
MYON=${1:-./myon}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
checks=0
check_error() {
    local source="$1" diagnostic="$2" engine rc
    printf 'module myon.stdio\nmodule myon.math\n%s\n' "$source" > "$scratch/input.myon"
    for engine in tree mvm; do
        rc=0
        if [ "$engine" = tree ]; then
            "$MYON" "$scratch/input.myon" >"$scratch/out" 2>"$scratch/err" || rc=$?
        else
            "$MYON" --run-mvm "$scratch/input.myon" >"$scratch/out" 2>"$scratch/err" || rc=$?
        fi
        if [ "$rc" -ne 1 ] || ! grep -Fq "$diagnostic" "$scratch/err"; then
            cat "$scratch/input.myon" "$scratch/err"
            echo "FAIL numeric error ($engine): expected exit 1 + '$diagnostic', got $rc" >&2
            exit 1
        fi
        checks=$((checks + 1))
    done
}
check_error 'minimum = -9223372036854775807 - 1; myon.print(-minimum)' 'integer overflow in negation'
check_error 'minimum = -9223372036854775807 - 1; myon.print(myon.math.abs(minimum))' 'integer overflow in myon.math.abs'
for arguments in 'minimum, 0' '0, minimum' 'minimum, minimum'; do
    check_error "minimum = -9223372036854775807 - 1; myon.print(myon.math.gcd($arguments))" 'integer overflow in myon.math.gcd'
done
for operation in int floor ceil round trunc; do
    for value in '9223372036854775808.0' '-9223372036854777856.0' 'myon.math.sqrt(-1.0)' 'myon.math.exp(1000.0)' '-myon.math.exp(1000.0)'; do
        if [ "$operation" = int ]; then
            call="int($value)"
            diagnostic='int(): numeric value out of range for int'
        else
            call="myon.math.$operation($value)"
            diagnostic="myon.math.$operation: numeric value out of range for int"
        fi
        check_error "myon.print($call)" "$diagnostic"
    done
done
for value in '9223372036854775808' '-9223372036854775809' '99999999999999999999999999999999999999'; do
    check_error "myon.print(int(\"$value\"))" 'int(): integer string out of range'
done
for engine in tree mvm; do
    for name in p_int_comparison_precision p_numeric_conversion_boundaries p_generic_comparison_lookahead; do
        if [ "$engine" = tree ]; then
            "$MYON" "tests/cases/$name.myon" > "$scratch/out"
        else
            "$MYON" --run-mvm "tests/cases/$name.myon" > "$scratch/out"
        fi
        diff -u "tests/cases/$name.out" "$scratch/out"
        checks=$((checks + 1))
    done
done
echo "numeric/parser boundary regression tests passed ($checks checks)"
