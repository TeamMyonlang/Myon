#!/usr/bin/env bash
set -eu
probe=$1
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
for mode in success language-error; do
    rc=0
    if [ "$mode" = success ]; then
        "$probe" > "$scratch/out" 2> "$scratch/err" || rc=$?
    else
        "$probe" language-error > "$scratch/out" 2> "$scratch/err" || rc=$?
    fi
    if [ "$rc" -ne 99 ] || ! grep -q 'LeakSanitizer: detected memory leaks' "$scratch/err"; then
        cat "$scratch/err" >&2
        echo "FAIL LSan self-test ($mode): expected leak report and exit 99, got $rc" >&2
        exit 1
    fi
done
echo 'LSan self-test passed (leaks override both success and language-error exits)'
