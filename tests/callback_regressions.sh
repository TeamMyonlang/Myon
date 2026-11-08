#!/usr/bin/env bash
# Saved bytecode must retain native callback behavior without a source AST.
set -eu
MYON=${1:-./myon}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
for name in p_native_callback_state p4_array_higher_order; do
    "$MYON" --compile "tests/cases/$name.myon" -o "$scratch/$name.myc"
    "$MYON" "$scratch/$name.myc" > "$scratch/out"
    diff -u "tests/cases/$name.out" "$scratch/out"
done
if [ "$(uname -s)" = Linux ]; then
    for name in p41_ffi_callback p_ffi_callback_reentry; do
        "$MYON" --compile "tests/cases/$name.myon" -o "$scratch/$name.myc"
        "$MYON" "$scratch/$name.myc" > "$scratch/out" 2> "$scratch/err"
        diff -u "tests/cases/$name.out" "$scratch/out"
    done
fi
# Cross-native recursion must honor a small C stack as well as the VM limit.
"$MYON" --compile tests/cases/p_native_callback_recursion.myon -o "$scratch/recursive.myc"
rc=0
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        # POSIX ulimit cannot resize a native Windows executable's PE stack.
        "$MYON" "$scratch/recursive.myc" > "$scratch/out" 2> "$scratch/err" || rc=$? ;;
    *)
        (ulimit -s 1024; "$MYON" "$scratch/recursive.myc") > "$scratch/out" 2> "$scratch/err" || rc=$? ;;
esac
if [ "$rc" -ne 1 ] || ! grep -q 'call stack too deep' "$scratch/err"; then
    cat "$scratch/err" >&2
    echo "FAIL native callback recursion: expected exit 1, got $rc" >&2
    exit 1
fi
echo 'saved bytecode native callback regression tests passed'
