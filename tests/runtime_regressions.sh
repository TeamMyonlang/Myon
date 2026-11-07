#!/usr/bin/env bash
# Strict status checks: a signal must never pass as an expected language error.
set -eu
MYON=${1:-./myon}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for engine in tree mvm; do
    status=0
    if [ "$engine" = tree ]; then
        "$MYON" tests/cases/p_async_recursion_limit.myon >"$tmp/log" 2>&1 || status=$?
        diagnostic='call stack too deep'
    else
        "$MYON" --run-mvm tests/cases/p_async_recursion_limit.myon >"$tmp/log" 2>&1 || status=$?
        diagnostic='call depth exceeded'
    fi
    if [ "$status" -ne 1 ] || ! grep -q "$diagnostic" "$tmp/log"; then
        cat "$tmp/log"
        echo "FAIL async recursion ($engine): expected status 1, got $status" >&2
        exit 1
    fi
done
for engine in tree mvm; do
    if [ "$engine" = tree ]; then
        "$MYON" tests/cases/p_async_depth_isolation.myon >"$tmp/out"
    else
        "$MYON" --run-mvm tests/cases/p_async_depth_isolation.myon >"$tmp/out"
    fi
    diff -u tests/cases/p_async_depth_isolation.out "$tmp/out"
done
echo 'async recursion status and isolation checks passed'
