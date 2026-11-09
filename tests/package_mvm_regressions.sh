#!/usr/bin/env bash
set -eu
MYON=$(cd "$(dirname "${1:-./myon}")" && pwd)/$(basename "${1:-./myon}")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cp -R tests/cases/pkgproj "$scratch/proj"
cat >> "$scratch/proj/.myon/packages/example-tools/modules/example/tools.myon" <<'MYON'
myon.func apply(f: func, x: int) ret int { ret f(x) }
myon.async myon.func later(x: int) ret int { ret x + 1 }
myon.struct Box {
    n: int
    myon.func get() ret int { ret self.n }
}
myon.func box(x: int) ret Box { ret Box(n=x) }
MYON
cat > "$scratch/proj/main.myon" <<'MYON'
module example.tools as tools
module myon.stdio
module myon.array
f = tools.square
myon.print(f(7))
myon.print(tools.apply(myon.lambda(x: int) ret int { ret x + 2 }, 5))
myon.print(myon.await tools.later(9))
b = tools.box(12)
myon.print(b.get())
xs = myon.array(int)
xs.push(3)
myon.print(xs.map(myon.lambda(x: int) ret int { ret tools.square(x) }))
MYON
printf '49\n7\n10\n12\n[9]\n' > "$scratch/expected"
for mode in tree mvm bytecode; do
    case "$mode" in
        tree) (cd /; "$MYON" "$scratch/proj/main.myon") > "$scratch/out" ;;
        mvm) (cd /; "$MYON" --run-mvm "$scratch/proj/main.myon") > "$scratch/out" ;;
        bytecode)
            "$MYON" --compile "$scratch/proj/main.myon" -o "$scratch/proj/main.myc"
            (cd /; "$MYON" "$scratch/proj/main.myc") > "$scratch/out" ;;
    esac
    diff -u "$scratch/expected" "$scratch/out"
done
for project in pkgproj_unlocked pkgproj_cycle; do
    for mode in mvm bytecode; do
        rc=0
        if [ "$mode" = mvm ]; then
            "$MYON" --run-mvm "tests/cases/$project/main.myon" > "$scratch/out" 2> "$scratch/err" || rc=$?
        else
            cp -R "tests/cases/$project" "$scratch/$project"
            "$MYON" --compile "$scratch/$project/main.myon" -o "$scratch/$project/main.myc"
            "$MYON" "$scratch/$project/main.myc" > "$scratch/out" 2> "$scratch/err" || rc=$?
        fi
        [ "$rc" -eq 1 ] || { cat "$scratch/err"; echo "FAIL package error: $project $mode $rc"; exit 1; }
    done
done
rc=0
"$MYON" --compile tests/cases/pkgproj_noalias/main.myon -o "$scratch/noalias.myc" > "$scratch/out" 2> "$scratch/err" || rc=$?
[ "$rc" -eq 65 ] && grep -q 'alias' "$scratch/err"
echo 'MVM package import regression tests passed'
