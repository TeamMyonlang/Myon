#!/usr/bin/env bash
# Exercise the actual harnesses against a fake interpreter in an isolated tree.
set -eu
ulimit -c 0
scratch=$(mktemp -d)
trap 'rc=$?; if [ "$rc" -ne 0 ] && [ -f "$scratch/log" ]; then cat "$scratch/log"; fi; rm -rf "$scratch"; exit "$rc"' EXIT
mkdir -p "$scratch/tests/cases"
cp tests/run_tests.sh tests/run_mvm_tests.sh "$scratch/tests/"
cat > "$scratch/myon" <<'FAKE'
#!/usr/bin/env bash
mode=tree
case "$1" in
    --version|-v) echo 'myon 1.0.0'; exit 0 ;;
    --run-mvm) mode=vm; shift ;;
    --compile) mode=compile; shift ;;
esac
name=$(basename "$1" .myon)
printf '%s %s\n' "$mode" "$name" >> calls
case "$name" in
    output_ok) echo A ;;
    output_error) echo A; exit 1 ;;
    output_crash) echo A; kill -SEGV $$ ;;
    runtime_error) exit 1 ;;
    source_error) exit 65 ;;
    error_crash) kill -SEGV $$ ;;
    error_abort) kill -ABRT $$ ;;
    error_timeout) exit 124 ;;
    error_missing) exit 66 ;;
    unsupported_compile|unsupported_runtime)
        if [ "$mode" != tree ]; then
            echo 'MVM does not support fixture feature' >&2
            [ "$name" = unsupported_compile ] && exit 65
            exit 1
        fi
        echo A ;;
    unsupported_tree_crash)
        if [ "$mode" = tree ]; then kill -SEGV $$; fi
        echo 'MVM does not support fixture feature' >&2
        exit 65 ;;
    unsupported_crash)
        echo 'MVM does not support fixture feature' >&2
        kill -SEGV $$ ;;
    *) exit 1 ;;
esac
FAKE
chmod +x "$scratch/myon"
for name in output_ok output_error output_crash unsupported_compile unsupported_runtime; do
    : > "$scratch/tests/cases/$name.myon"
    printf 'A\n' > "$scratch/tests/cases/$name.out"
done
for name in runtime_error source_error error_crash error_abort error_timeout error_missing unsupported_crash unsupported_tree_crash; do
    : > "$scratch/tests/cases/$name.myon"
    : > "$scratch/tests/cases/$name.err"
done
: > "$scratch/tests/cases/missing_fixture.myon"

for harness in run_tests run_mvm_tests; do
    : > "$scratch/calls"
    rc=0
    bash "$scratch/tests/$harness.sh" >"$scratch/log" 2>&1 || rc=$?
    [ "$rc" -ne 0 ]
    for name in output_error output_crash error_crash error_abort error_timeout error_missing unsupported_crash unsupported_tree_crash missing_fixture; do
        if ! grep -Eq "  FAIL $name( |$)" "$scratch/log"; then
            cat "$scratch/log"
            echo "FAIL $harness accepted $name" >&2
            exit 1
        fi
    done
    for name in output_ok runtime_error source_error; do
        grep -Eq "  ok   $name( |$)" "$scratch/log"
    done
    if [ "$harness" = run_mvm_tests ]; then
        for name in unsupported_compile unsupported_runtime; do
            grep -Eq "  excl $name( |$)" "$scratch/log"
        done
        # Every case with a fixture runs exactly once per engine, including
        # unsupported cases. Missing fixtures never execute the interpreter.
        for source in "$scratch"/tests/cases/*.myon; do
            name=$(basename "$source" .myon)
            expected=1
            [ "$name" = missing_fixture ] && expected=0
            for mode in tree vm; do
                count=$(grep -c "^$mode $name$" "$scratch/calls" || true)
                [ "$count" -eq "$expected" ] || {
                    echo "FAIL $name executed $count times in $mode (expected $expected)" >&2
                    exit 1
                }
            done
        done
        if grep -q '^compile ' "$scratch/calls"; then
            echo 'FAIL unexpected compile preflight' >&2
            exit 1
        fi
    fi
done
echo 'test harness regression tests passed'
