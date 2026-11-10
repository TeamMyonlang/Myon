#!/usr/bin/env bash
set -eu
if command -v timeout >/dev/null 2>&1; then TIMEOUT=timeout; else TIMEOUT=gtimeout; fi
MYON=$(cd "$(dirname "${1:-./myon}")" && pwd)/$(basename "${1:-./myon}")
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/input" <<'MYON'
module myon.stdio
module myon.array
x = 1
myon.print(x)
myon.func add(y: int) ret int { ret x + y }
x = 5
myon.print(add(2))
myon.func make() ret func {
 n = 10
 ret myon.lambda(y: int) ret int { n += y; ret n }
}
f = make()
myon.print(f(2))
ghost = 99; Missing<int>()
myon.print(x)
myon.print(ghost)
myon.print(1 / 0)
myon.print(f(3))
myon.func add(y: int) ret int { ret y * 3 }
myon.print(add(2))
myon.struct Box { n: int; myon.func get() ret int { ret self.n } }
b = Box(n=42)
myon.print(b.get())
myon.struct Box { other: str }
myon.print(b.get())
xs = myon.array(int)
xs.push(2)
myon.print(xs.map(f))
module external.modlib.math as math
myon.print(math.square(5))
module external.nonexistent as missing
module external.nonexistent as missing
myon.print(f(1))
module myon.time
myon.async myon.func later() ret int { myon.time.sleep_ms(1); ret add(3) }
myon.print(myon.await later())
myon.print("done")
quit
MYON
(cd tests/cases; "$MYON" --repl-mvm < "$scratch/input") > "$scratch/out" 2> "$scratch/err"
sed -e '/^MVM backend\./d' -e '/^Myon REPL\./d' -e 's/myon> //g' -e 's/\.\.\.> //g' -e '/^$/d' "$scratch/out" > "$scratch/actual"
printf '1\n7\n12\n5\n15\n6\n42\n42\n[17]\n25\n18\n9\ndone\n' > "$scratch/expected"
diff -u "$scratch/expected" "$scratch/actual"
grep -q 'unknown generic type' "$scratch/err"
grep -q 'undefined external binding.*ghost' "$scratch/err"
grep -q 'division by zero' "$scratch/err"
grep -q 'redefinition of struct' "$scratch/err"
[ "$(grep -c 'cannot open module file' "$scratch/err")" -eq 2 ]
# Old function diagnostics use the retained defining input, not the caller text.
printf 'module myon.stdio\nmyon.func fail() ret int { ret 1 / 0 }\nfail()\nmyon.print(7)\nquit\n' | "$MYON" --run-mvm > "$scratch/out" 2> "$scratch/err"
grep -Fq 'myon.func fail() ret int { ret 1 / 0 }' "$scratch/err"
grep -q 'myon> 7' "$scratch/out"
# Conflicting execution modes fail explicitly; EOF closes a clean session.
rc=0
"$MYON" --repl-mvm tests/cases/step3_hello.myon > "$scratch/out" 2> "$scratch/err" || rc=$?
[ "$rc" -eq 64 ]
"$MYON" --repl-mvm < /dev/null > "$scratch/out"
echo 'MVM REPL regression tests passed'
# Keep a suspended server alive while subsequent inputs grow module tables.
cat > "$scratch/server" <<'MYON'
module myon.stdio
module myon.http
prefix = "before"
myon.func handler(method: str, path: str, body: str) ret str { ret prefix + path }
myon.async myon.func server() ret int { myon.http.serve(19127, handler); ret 0 }
task = server()
myon.func extra1() ret str { ret "constant pool growth one" }
myon.func extra2() ret str { ret "constant pool growth two" }
myon.func extra3() ret str { ret "constant pool growth three" }
prefix = "after"
myon.async myon.func client() ret int { body, status, err = myon.http.get("http://127.0.0.1:19127/ok"); myon.print(status); myon.print(body); ret status }
tc = client()
myon.print(1 / 0)
module myon.time
myon.async myon.func later() ret int { myon.time.sleep_ms(1); ret 9 }
myon.print(myon.await later())
quit
MYON
"$TIMEOUT" 30 "$MYON" --repl-mvm < "$scratch/server" > "$scratch/out" 2> "$scratch/err"
sed -e '/^MVM backend\./d' -e '/^Myon REPL\./d' -e 's/myon> //g' -e 's/\.\.\.> //g' -e '/^$/d' "$scratch/out" > "$scratch/actual"
printf '200\nafter/ok\n9\n' > "$scratch/expected"
diff -u "$scratch/expected" "$scratch/actual"
grep -q 'division by zero' "$scratch/err"
echo 'MVM REPL daemon preservation and cancellation tests passed'
