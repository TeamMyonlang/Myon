#!/usr/bin/env bash
set -eu
MYON=$(cd "$(dirname "${1:-./myon}")" && pwd)/$(basename "${1:-./myon}")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/lib.myon" <<'MYON'
count = 10
myon.func read_count() ret int { ret count }
myon.func square(x: int) ret int { ret x * x }
myon.func apply(f: func) ret int { ret f(2) }
myon.func pair() ret int, int { ret 3, 4 }
MYON
cat > "$scratch/main.myon" <<'MYON'
module external.lib
module myon.stdio
module myon.array
myon.print(square(3))
count = 11
myon.print(read_count())
myon.func increment() ret int { count += 1; ret count }
myon.print(increment())
myon.print(read_count())
myon.func set_count() ret int { count = 30; ret count }
myon.print(set_count())
myon.print(read_count())
myon.func make() ret func {
    local = 4
    ret myon.lambda(x: int) ret int { local = local + x; ret local }
}
f = make()
local = 100
myon.print(f(1))
myon.print(local)
myon.print(apply(myon.lambda(x: int) ret int { ret x + 5 }))
myon.func assign_pair() ret int {
    count, second = pair()
    ret count + second
}
myon.print(assign_pair())
myon.print(read_count())
MYON
printf '9\n11\n12\n12\n30\n30\n5\n100\n7\n7\n3\n' > "$scratch/expected"
for mode in tree mvm bytecode; do
 case "$mode" in
 tree) (cd /; "$MYON" "$scratch/main.myon") > "$scratch/out" ;;
 mvm) (cd /; "$MYON" --run-mvm "$scratch/main.myon") > "$scratch/out" ;;
 bytecode) "$MYON" --compile "$scratch/main.myon" -o "$scratch/main.myc"
 (cd /; "$MYON" "$scratch/main.myc") > "$scratch/out" ;;
 esac
 diff -u "$scratch/expected" "$scratch/out"
done
# Aliased imported function values and source-independent bytecode.
cp -R tests/cases/modlib "$scratch/modlib"
cp tests/cases/p_module_alias.myon "$scratch/alias.myon"
"$MYON" --compile "$scratch/alias.myon" -o "$scratch/alias.myc"
"$MYON" "$scratch/alias.myc" > "$scratch/out"
diff -u tests/cases/p_module_alias.out "$scratch/out"
for source in 'module external.missing as m' 'module external.a as a'; do
 printf '%s\n' "$source" > "$scratch/main.myon"
 printf 'module external.b as b\n' > "$scratch/a.myon"
 printf 'module external.a as a\n' > "$scratch/b.myon"
 rc=0
 "$MYON" --run-mvm "$scratch/main.myon" > "$scratch/out" 2> "$scratch/err" || rc=$?
 [ "$rc" -eq 1 ] || { cat "$scratch/err"; exit 1; }
done
echo 'external import regression tests passed'
