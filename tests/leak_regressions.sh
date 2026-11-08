#!/usr/bin/env bash
set -eu
MYON=${1:-./myon_asan}
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export LSAN_OPTIONS=exitcode=99
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1
if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_BIN=timeout
elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_BIN=gtimeout
else
    echo 'error: GNU timeout is required' >&2
    exit 1
fi
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
count=0
run_case() {
    local src=$1 engine=$2 expected=$3 rc=0
    local -a flags=()
    [ "$engine" = tree ] || flags=(--run-mvm)
    "$TIMEOUT_BIN" 30 "$MYON" "${flags[@]}" "$src" > "$scratch/out" 2> "$scratch/err" || rc=$?
    if [ "$rc" -ne "$expected" ]; then
        cat "$scratch/err" >&2
        echo "FAIL leak check: $src ($engine), expected $expected, got $rc" >&2
        exit 1
    fi
    count=$((count + 1))
}
# Error fixtures have separate known unwinding leaks. Check every success
# fixture, including HTTP daemon shutdown, without suppressing leak reports.
for expected in tests/cases/*.out; do
    src=${expected%.out}.myon
    run_case "$src" tree 0
    case "$(basename "$src" .myon)" in
        p41_ffi_callback|p4_array_higher_order|p_module_alias) continue ;;
    esac
    run_case "$src" mvm 0
done
cat > "$scratch/ready.myon" <<'MYON'
module myon.stdio
myon.async myon.func pending(payload: str) ret str {
    ret payload
}
task = pending(str("retained argument"))
myon.print(1 / 0)
MYON
cat > "$scratch/suspended.myon" <<'MYON'
module myon.time
module myon.stdio
myon.async myon.func pending(payload: str) ret str {
    myon.func capture() ret str { ret payload }
    myon.time.sleep_ms(60000)
    ret capture()
}
myon.async myon.func fail() ret int {
    myon.print(1 / 0)
    ret 0
}
task = pending(str("retained argument"))
myon.await fail()
MYON
for engine in tree mvm; do
    run_case "$scratch/ready.myon" "$engine" 1
    grep -q 'division by zero' "$scratch/err"
    run_case "$scratch/suspended.myon" "$engine" 1
    grep -q 'division by zero' "$scratch/err"
done
printf 'leak regression tests passed (%s engine executions)\n' "$count"
