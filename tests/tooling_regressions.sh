#!/usr/bin/env bash
set -eu
MYON=${1:-./myon}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# A saved module retains declarations, including inherited fields and methods.
mkdir "$tmp/a" "$tmp/b"
cat > "$tmp/a/report.myon" <<'MYON'
module myon.stdio
myon.struct Entry myon.extends Base {
    total: int
    myon.func render() ret str { ret self.label() + "," + str(self.total) }
}
myon.struct Base {
    name: str
    myon.func label() ret str { ret self.name }
}
entry = Entry(name="north", total=128)
myon.print("name,total")
myon.print(entry.render())
MYON
printf 'name,total\nnorth,128\n' > "$tmp/expected"
"$MYON" --check "$tmp/a/report.myon" > "$tmp/out" 2> "$tmp/err"
[ ! -s "$tmp/out" ]
[ ! -s "$tmp/err" ]
[ ! -e "$tmp/a/report.myc" ]
"$MYON" --check - < "$tmp/a/report.myon"
rc=0
printf 'myon.func f() ret int {\n' | "$MYON" --check - > "$tmp/out" 2> "$tmp/err" || rc=$?
[ "$rc" -eq 65 ]
[ ! -s "$tmp/out" ]
grep -q 'error' "$tmp/err"
cp "$tmp/a/report.myon" "$tmp/b/report.myon"
touch -t 203101010000 "$tmp/b/report.myon"
"$MYON" --compile --reproducible "$tmp/a/report.myon" -o "$tmp/a/report.myc"
"$MYON" --compile "$tmp/b/report.myon" --reproducible -o "$tmp/b/report.myc"
cmp "$tmp/a/report.myc" "$tmp/b/report.myc"

# Saved bytecode works in a process that has no access to the original source.
mv "$tmp/b/report.myon" "$tmp/b/report.src"
"$MYON" "$tmp/b/report.myc" > "$tmp/out"
cmp "$tmp/expected" "$tmp/out"
"$MYON" --compile "$tmp/a/report.myon" -o "$tmp/a/report.myc"
touch -t 203101010000 "$tmp/a/report.myon"
"$MYON" --strict-stale "$tmp/a/report.myc" > "$tmp/out" 2> "$tmp/err"
cmp "$tmp/expected" "$tmp/out"
[ ! -s "$tmp/err" ]
sed 's/128/129/' "$tmp/b/report.src" > "$tmp/a/report.myon"
rc=0
"$MYON" --strict-stale "$tmp/a/report.myc" > "$tmp/out" 2> "$tmp/err" || rc=$?
[ "$rc" -eq 65 ]
[ ! -s "$tmp/out" ]
grep -q 'source content has changed' "$tmp/err"
cp "$tmp/b/report.src" "$tmp/a/report.myon"

# Receipt counts are identical on the two engines and independent of stdout.
for mode in tree memory saved; do
    case "$mode" in
        tree) "$MYON" --receipt "$tmp/a/report.myon" > "$tmp/out" 2> "$tmp/err"; engine=tree-walk ;;
        memory) "$MYON" --receipt --run-mvm "$tmp/a/report.myon" > "$tmp/out" 2> "$tmp/err"; engine=mvm ;;
        saved) "$MYON" --receipt "$tmp/a/report.myc" > "$tmp/out" 2> "$tmp/err"; engine=mvm ;;
    esac
    cmp "$tmp/expected" "$tmp/out"
    grep -Eq "^engine +$engine$" "$tmp/err"
    grep -Eq '^explicit conversions +1$' "$tmp/err"
    grep -Eq '^stdout bytes +21$' "$tmp/err"
    grep -Eq '^exit status +0$' "$tmp/err"
done

# A 1.4 artifact without a struct table continues to load.
printf 'module myon.stdio\nmyon.print(7)\n' > "$tmp/legacy.myon"
"$MYON" --compile "$tmp/legacy.myon" -o "$tmp/current.myc"
length=$(wc -c < "$tmp/current.myc")
dd if="$tmp/current.myc" of="$tmp/legacy.myc" bs=1 count="$((length - 4))" 2>/dev/null
printf '\004' | dd of="$tmp/legacy.myc" bs=1 seek=5 conv=notrunc 2>/dev/null
[ "$("$MYON" "$tmp/legacy.myc")" = 7 ]

printf 'sample,current,previous\n1,20,\n2,21,20\n3,19,21\n' > "$tmp/expected"
"$MYON" examples/follow.myon > "$tmp/out"
cmp "$tmp/expected" "$tmp/out"
"$MYON" --run-mvm examples/follow.myon > "$tmp/out"
cmp "$tmp/expected" "$tmp/out"

printf 'ingredient,amount,unit\nflour,150,g\negg,3/2,piece\n' > "$tmp/expected"
"$MYON" examples/prep.myon examples/recipe.json 3 > "$tmp/out"
cmp "$tmp/expected" "$tmp/out"
"$MYON" --compile examples/prep.myon -o "$tmp/prep.myc"
"$MYON" "$tmp/prep.myc" examples/recipe.json 3 > "$tmp/out"
cmp "$tmp/expected" "$tmp/out"
echo 'Saved bytecode, CLI, and example regressions passed'
