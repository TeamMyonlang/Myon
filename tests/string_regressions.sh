#!/usr/bin/env bash
set -eu
MYON=${1:-./myon}
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/input.myon" <<'MYON'
module myon.stdio
n = 7
myon.write("a\nb\rc\td\"e\\f|\\n|\q|\0|{{braces}}|{n}\n")
myon.write("{n}\t{{x}}\\\"\n")
myon.write("日本語\n")
myon.write("\n")
myon.write("\\")
text = str("\\n")
myon.write("|{text}|")
MYON
printf 'a\nb\rc\td"e\\f|\\n|\\q|\\0|{braces}|7\n7\t{x}\\"\n日本語\n\n\\|\\n|' > "$scratch/expected"
for engine in tree mvm bytecode; do
    case "$engine" in
        tree) "$MYON" "$scratch/input.myon" > "$scratch/out" ;;
        mvm) "$MYON" --run-mvm "$scratch/input.myon" > "$scratch/out" ;;
        bytecode)
            "$MYON" --compile "$scratch/input.myon" -o "$scratch/input.myc"
            "$MYON" "$scratch/input.myc" > "$scratch/out" ;;
    esac
    if ! cmp "$scratch/expected" "$scratch/out"; then
        echo "FAIL string escape bytes ($engine)" >&2
        exit 1
    fi
done
# A trailing backslash or escaped closing quote still needs a real terminator.
for source in 'myon.write("unterminated\' 'myon.write("unterminated\")'; do
    printf '%s' "$source" > "$scratch/input.myon"
    for engine in tree mvm; do
        rc=0
        if [ "$engine" = tree ]; then
            "$MYON" "$scratch/input.myon" > "$scratch/out" 2> "$scratch/err" || rc=$?
        else
            "$MYON" --run-mvm "$scratch/input.myon" > "$scratch/out" 2> "$scratch/err" || rc=$?
        fi
        [ "$rc" -eq 65 ]
        grep -q 'unterminated string literal' "$scratch/err"
    done
done
echo 'string escape byte and termination checks passed'
