#!/usr/bin/env bash
set -eu
# Diagnostics contain UTF-8 bytes even in native Windows CRT output. Compare
# those bytes directly; MSYS2's locale-aware grep can reject non-BMP patterns.
export LC_ALL=C
MYON=${1:-./myon}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT

check_lexical() {
    local input="$1" expected="$2" rc=0
    printf '%s' "$input" > "$scratch/input.myon"
    LC_ALL=C "$MYON" --tokens "$scratch/input.myon" >"$scratch/out" 2>"$scratch/err" || rc=$?
    if [ "$rc" -ne 65 ] || ! grep -Fq "$expected" "$scratch/err"; then
        cat "$scratch/err"
        echo "FAIL lexical diagnostic: expected '$expected', exit 65; got $rc" >&2
        exit 1
    fi
}
check_lexical 'あ' "unexpected character 'あ' (U+3042)"
check_lexical 'é' "unexpected character 'é' (U+00E9)"
check_lexical '😀' "unexpected character '😀' (U+1F600)"
check_lexical '@' "unexpected character '@' (0x40)"
check_lexical $'\xe3\x81' 'unexpected byte 0xe3 (invalid UTF-8)'
check_lexical $'\xc0\x80' 'unexpected byte 0xc0 (invalid UTF-8)'
check_lexical $'\xed\xa0\x80' 'unexpected byte 0xed (invalid UTF-8)'
check_lexical $'\xf4\x90\x80\x80' 'unexpected byte 0xf4 (invalid UTF-8)'

printf 'myon.print(1 / 0)\n' > "$scratch/input.myon"
for engine in tree mvm; do
    rc=0
    if [ "$engine" = tree ]; then
        "$MYON" "$scratch/input.myon" >"$scratch/out" 2>"$scratch/$engine.err" || rc=$?
    else
        "$MYON" --run-mvm "$scratch/input.myon" >"$scratch/out" 2>"$scratch/$engine.err" || rc=$?
    fi
    [ "$rc" -eq 1 ]
    grep -q '^myon: runtime error at line 1:' "$scratch/$engine.err"
done
cmp "$scratch/tree.err" "$scratch/mvm.err"
echo 'diagnostic regression tests passed'
