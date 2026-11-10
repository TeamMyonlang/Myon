#!/usr/bin/env bash
set -eEu
MYON=${1:-./myon}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
report_failure() {
    local rc=$1 line=$2 command=$3
    echo "FAIL module regression at line $line (exit $rc): $command" >&2
    for name in input.myon compile err repl.err; do
        if [ -s "$scratch/$name" ]; then
            echo "--- $name ---" >&2
            cat "$scratch/$name" >&2
        fi
    done
    return "$rc"
}
trap 'report_failure "$?" "$LINENO" "$BASH_COMMAND"' ERR
checks=0
run() {
    local engine=$1 expected=$2 rc=0
    case "$engine" in
        tree) "$MYON" "$scratch/input.myon" > "$scratch/out" 2> "$scratch/err" || rc=$? ;;
        mvm) "$MYON" --run-mvm "$scratch/input.myon" > "$scratch/out" 2> "$scratch/err" || rc=$? ;;
        bytecode)
            "$MYON" --compile "$scratch/input.myon" -o "$scratch/input.myc" > "$scratch/compile" 2>&1
            "$MYON" "$scratch/input.myc" > "$scratch/out" 2> "$scratch/err" || rc=$? ;;
    esac
    if [ "$rc" -ne "$expected" ]; then
        cat "$scratch/input.myon" "$scratch/err" >&2
        echo "FAIL module gate ($engine): expected $expected, got $rc" >&2
        exit 1
    fi
    checks=$((checks + 1))
}
while IFS='|' read -r module source; do
    for engine in tree mvm bytecode; do
        printf '%s\n' "$source" > "$scratch/input.myon"
        run "$engine" 1
        grep -Fq "module '$module' is not declared" "$scratch/err"
        printf '%s\nmodule %s\nmodule %s\n' "$source" "$module" "$module" > "$scratch/input.myon"
        run "$engine" 0 # forward declarations and duplicates
    done
done <<'CASES'
myon.stdio|myon.print(1)
myon.math|x = myon.math.abs(-1)
myon.string|x = myon.string.upper("café")
myon.file|x = myon.file.exists("missing-module-test-file")
myon.ffi|h, err = myon.ffi.load("missing-module-test-library")
myon.time|x = myon.time.now()
myon.random|x = myon.random.int(1, 1)
myon.net|s, err = myon.net.tcp_socket()
myon.http|body, status, err = myon.http.get("invalid-url")
myon.array|x = myon.array(int)
myon.map|x = myon.map(str, int)
CASES
for engine in tree mvm bytecode; do
    cat > "$scratch/input.myon" <<'MYON'
module myon.stdio
myon.func side_effect() ret int { myon.print("unexpected"); ret -1 }
x = myon.math.abs(side_effect())
MYON
    run "$engine" 1
    [ ! -s "$scratch/out" ]
    grep -Fq "module 'myon.math' is not declared" "$scratch/err"
    cat > "$scratch/input.myon" <<'MYON'
module myon.stdio
myon.if true { module myon.math }
MYON
    run "$engine" 1
    grep -Fq 'module declarations are only allowed at top level' "$scratch/err"
done
# Source cannot activate modules through compiler-only helper names.
printf 'myon.__module_enable("myon.math")\nx = myon.math.abs(-1)\n' > "$scratch/input.myon"
for engine in tree mvm; do
    rc=0
    if [ "$engine" = tree ]; then
        "$MYON" "$scratch/input.myon" > "$scratch/out" 2> "$scratch/err" || rc=$?
        [ "$rc" -eq 1 ]
    else
        "$MYON" --run-mvm "$scratch/input.myon" > "$scratch/out" 2> "$scratch/err" || rc=$?
        [ "$rc" -eq 65 ]
        grep -q 'reserved MVM intrinsic' "$scratch/err"
    fi
    [ ! -s "$scratch/out" ]
done
# Core scalar operations and collection methods do not themselves need imports.
printf 'x = int("3"); y = str(x); z = char(65)\n' > "$scratch/input.myon"
for engine in tree mvm bytecode; do run "$engine" 0; done
cat > "$scratch/input.myon" <<'MYON'
module myon.stdio
myon.func size(items: myon.array(str)) ret int { ret items.length() }
x = size(myon.argv())
MYON
for engine in tree mvm bytecode; do run "$engine" 0; done
# Session declarations survive a failed REPL input and remain available later.
printf 'module myon.stdio\nmyon.print(myon.math.abs(-9))\nmodule myon.math\nmyon.print(myon.math.abs(-9))\nquit\n' |
    "$MYON" > "$scratch/repl.out" 2> "$scratch/repl.err"
grep -Fq "module 'myon.math' is not declared" "$scratch/repl.err"
grep -Eq 'myon> 9$' "$scratch/repl.out"
[ "$(grep -Fc "module 'myon.math' is not declared" "$scratch/repl.err")" -eq 1 ]
# A returned closure remains callable across REPL inputs and error recovery.
cat > "$scratch/session" <<'MYON'
module myon.stdio
myon.func make() ret func { n = 0; ret myon.lambda(x: int) ret int { n = n + x; ret n } }
f = make()
myon.print(f(1))
myon.print(1 / 0)
myon.print(f(2))
quit
MYON
"$MYON" < "$scratch/session" > "$scratch/repl.out" 2> "$scratch/repl.err"
grep -Eq 'myon> 1$' "$scratch/repl.out"
grep -Eq 'myon> 3$' "$scratch/repl.out"
[ "$(grep -Fc 'division by zero' "$scratch/repl.err")" -eq 1 ]
# Imported declarations are shared by the interpreter session. Parent builtin
# declarations are enabled before an external module's top-level side effects.
cat > "$scratch/lib.myon" <<'MYON'
myon.print("loaded")
myon.func number() ret int { ret myon.math.abs(-9) }
module myon.math
MYON
cat > "$scratch/input.myon" <<'MYON'
module external.lib as lib
module myon.stdio
myon.print(lib.number())
myon.print(myon.math.abs(-5))
MYON
run tree 0
printf 'loaded\n9\n5\n' > "$scratch/expected"
cmp "$scratch/expected" "$scratch/out"
# Module activation is part of MYC1 1.4 semantics: reject older and future minors.
printf 'module myon.stdio\nmyon.print(7)\n' > "$scratch/input.myon"
"$MYON" --compile "$scratch/input.myon" -o "$scratch/current.myc"
for minor in '\003' '\005'; do
    cp "$scratch/current.myc" "$scratch/old.myc"
    printf '%b' "$minor" | dd of="$scratch/old.myc" bs=1 seek=5 conv=notrunc 2>/dev/null
    rc=0
    "$MYON" "$scratch/old.myc" > "$scratch/out" 2> "$scratch/err" || rc=$?
    [ "$rc" -eq 65 ]
done
printf 'builtin module regression tests passed (%s engine executions)\n' "$checks"
