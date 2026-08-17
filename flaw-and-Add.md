# flaw-and-Add — コードレビュー結果（欠陥と追加提案）

このドキュメントは、`main` ブランチの `src/` に対するマニュアルコードレビューと、
実際に `myon` をビルドして怪しい箇所を「つついた」動的検証（ASan / UBSan 併用）の
結果をまとめたものです。

- **今回の方針**: バグは「レビュー中に実際に踏んだもの」のみを記載します。
  （網羅的なバグ探索は目的外。既存の `known-issues.md` は別レビューの記録です。）
- **レビュー日**: 2026-08-17
- **対象**: `main` ブランチ / `src/` 全体（ツリーウォーク実装 & MVM 経路の共通 stdlib）
- **環境**: Linux, `cc (gcc)`。`make` で警告ゼロ・全テスト前提。

## 検証方法（再現手順）

```bash
# 通常ビルド
make

# ASan/UBSan 版（裏取り用）
cc -std=c11 -g -O0 -fsanitize=address,undefined -fno-omit-frame-pointer \
   -Isrc src/*.c -o myon_asan -lm -lssl -lcrypto -ldl
```

各欠陥の「再現」節にある `.myon` を `./myon <file>` および `./myon_asan <file>` で
実行して確認しました。

---

## A. 見つけた欠陥（今回実際に踏んだもの）

深刻度の凡例: 🔴 重大（メモリ破壊 / クラッシュ） / 🟠 中（誤結果・整合性） / 🟡 低（表示・往復性）

> **修正ステータス（2026-08-17 更新）**: A-1〜A-6 は本 PR で**すべて修正済み**。
> 各項目末尾に ✅ 修正内容を追記した。B 群（Add）は今回対象外。

### A-1. 🔴 `myon.string.repeat` の乗算オーバーフローによるヒープバッファオーバーフロー

- **場所**: `src/interpreter.c` `myon.string.repeat`（およそ L2617–2637）
- **内容**:
  ```c
  size_t unit  = strlen(cs);
  size_t total = unit * (size_t)n;          // ← オーバーフロー無防備
  char  *buf   = (char *)myon_xmalloc(total + 1);
  for (long long i = 0; i < n; i++)
      memcpy(buf + (size_t)i * unit, cs, unit);   // ← 小さすぎる buf に書き続ける
  ```
  `unit * n` が `size_t`（64bit）を回り込むと、実際に必要な量より**遥かに小さい**
  バッファを確保してしまい、`memcpy` ループがその外に書き込みます。
- **影響**: **ヒープバッファオーバーフロー → SIGSEGV（メモリ破壊）**。
  ツリーウォーク実行・`.myc`（MVM）実行の**両方**で再現（共通 stdlib を通るため）。
- **再現**:
  ```
  system myon.useversion=1
  module myon.stdio
  module myon.string
  # unit=16, n=2^60 → 16 * 2^60 = 2^64 が size_t で 0 に回り込む
  r, e = myon.string.repeat(str("0123456789ABCDEF"), 1152921504606846976)
  myon.print(myon.string.length(r))
  ```
  - 通常ビルド: `Segmentation fault (exit 139)`
  - ASan: `AddressSanitizer: heap-buffer-overflow ... WRITE of size 16 ... in memcpy`
- **修正案**: 確保前に乗算オーバーフローを検査する。
  ```c
  if (n != 0 && unit > (SIZE_MAX - 1) / (size_t)n) { /* error を返す */ }
  ```
  または `__builtin_mul_overflow` を使い、越えたら `repeat: result too large` の
  `error` 値を返す（既存の tuple/error 慣習に合わせる）。加えて `n` の上限を
  設けるのが安全。
- **✅ 修正済み**: `__builtin_mul_overflow(unit, n, &total)` で確保前に検査し、
  越えたら `myon.string.repeat: result too large` の `error` を返すようにした。
  ASan で heap-buffer-overflow が消滅、クラッシュせずエラー値が返ることを確認。

---

### A-2. 🔴 `myon.string.substring` の範囲チェックにおける符号付き整数オーバーフロー

- **場所**: `src/interpreter.c` `myon.string.substring`（L2410–2434、特に L2421 と L2427）
- **内容**:
  ```c
  if (start < 0 || len < 0 || start + len > nchars) { ... }   // start+len が UB
  ...
  long long eoff = utf8_byte_offset(cs, start + len);         // 同じく UB
  ```
  `start` と `len` がともに巨大な正の `long long`（例: 2^62）だと `start + len` が
  **符号付きオーバーフロー（C では未定義動作）**を起こし、負値に回り込んで
  範囲チェックをすり抜けます。その後 `eoff - boff = nbytes` が巨大になり
  `myon_xmalloc` が OOM 経路で `exit(70)`。
- **影響**: UB ＋ インタプリタ全体の異常終了（DoS）。
- **再現**:
  ```
  r, e = myon.string.substring(str("hello"), 4611686018427387904, 4611686018427387904)
  ```
  - 通常ビルド: `myon: out of memory` → `exit 70`
  - UBSan: `runtime error: signed integer overflow: ... + ... cannot be represented in type 'long long int'`（L2421 / L2427）
    ＋ `AddressSanitizer: allocation-size-too-big`
- **修正案**: 加算前に上限を検査する。
  ```c
  if (start < 0 || len < 0 || start > nchars || len > nchars - start) { /* out of bounds */ }
  ```
  （`nchars - start` は `start <= nchars` を先に確認していれば安全）
- **✅ 修正済み**: 上記の各境界を個別に検査する形へ書き換え、`start + len` の
  符号付きオーバーフロー（UB）を排除。UBSan/ASan ともにクリーン、
  巨大入力で `range out of bounds` エラーが返ることを確認。

---

### A-3. 🔴/🟠 `myon.array.slice` の範囲チェックにおける符号付き整数オーバーフロー

- **場所**: `src/interpreter.c` 配列メソッド `slice`（L2832–2851、特に L2842 と L2848）
- **内容**: A-2 と同一パターン。
  ```c
  if (start < 0 || len < 0 || start + len > (long long)a->count) { ... }  // UB
  for (long long i = start; i < start + len; i++) ...                     // UB
  ```
- **影響**: UB。今回の入力ではクラッシュせず、範囲外要求が黙って「成功・空配列」
  として扱われる**整合性の欠陥**（A-2 の substring では OOM クラッシュに直結）。
- **再現**:
  ```
  a = myon.array(int)
  a.push(1)  a.push(2)  a.push(3)
  r, e = a.slice(4611686018427387904, 4611686018427387904)
  myon.print(e)   # → myon.nil（本来は out of bounds エラーであるべき）
  ```
  - UBSan: `runtime error: signed integer overflow`（L2842 / L2848）
- **修正案**: A-2 と同じく `len > count - start` 形式へ書き換える。
- **✅ 修正済み**: `count = (long long)a->count` を導入し、境界チェックを
  `start > count || len > count - start` に、ループ上限を事前計算した `end` に
  変更。UBSan クリーン、巨大入力で `range out of bounds` を返すようにした。

> 補足: `src/interpreter.c` の算術演算（`int_arith`）は `__builtin_add/sub/mul_overflow`
> と `LLONG_MIN / -1` まで丁寧に守られています。**にもかかわらず** stdlib の
> 境界チェック（A-1〜A-3）だけが生の `+` / `*` を使っており、防御が一貫していません。

---

### A-4. 🟠 整数リテラルのオーバーフローが黙ってクランプされる

- **場所**: `src/parser.c` `parse_primary`（L253–261）
- **内容**: `strtoll(..., 0)` / `strtoll(..., 8)` の戻り値をそのまま採用し、`errno`
  （`ERANGE`）を検査していません。範囲外の整数リテラルは `LLONG_MAX`/`LLONG_MIN`
  にサチュレートし、**エラーにならず壊れた値**になります。
- **影響**: 誤った値を静かに生成。README「Step 12 整数オーバーフローチェック
  （実行時エラー）」の趣旨とも矛盾（実行時演算は守るのにリテラルは素通り）。
- **再現**:
  ```
  a = 99999999999999999999999999
  myon.print(a)              # → 9223372036854775807
  c = 0xFFFFFFFFFFFFFFFFFF
  myon.print(c)              # → 9223372036854775807
  ```
- **修正案**: `errno = 0` を設定して `strtoll` 後に `errno == ERANGE` を検査し、
  越えていたらパースエラー（`integer literal out of range`）にする。
- **✅ 修正済み**: `parse_primary` で `errno = 0` 設定後に `errno == ERANGE` を検査し、
  越えていたら `perror_at(..., "integer literal out of range")` で構文エラーに
  するよう変更（10 進・8 進・16 進すべて対象）。

---

### A-5. 🟠 `myon.string.to_int` / `to_float` が範囲外(ERANGE)を成功として返す

- **場所**: `src/interpreter.c` `myon.string.to_int`（L2640–2653）/ `to_float`（L2657–2670）
- **内容**: どちらも `errno = 0` を**設定しているのに** `errno == ERANGE` を
  **一度も確認していません**。オーバーフローする文字列でも、サチュレートした値を
  「成功（error = `myon.nil`）」として返します。書きかけの検証が残っている典型。
- **影響**: 数値変換の誤り。呼び出し側は成功と誤認する。
- **再現**:
  ```
  v, e = myon.string.to_int(str("99999999999999999999"))
  myon.print(v)   # → 9223372036854775807
  myon.print(e)   # → myon.nil（本来はエラー）
  ```
- **修正案**: `end`/入力の検証に加えて `errno == ERANGE` を分岐に足し、
  越えたら `to_int: out of range` の `error` を返す（`to_float` も同様、`HUGE_VAL`）。
- **✅ 修正済み**: 両関数に `errno == ERANGE` 分岐を追加し、越えたら
  `myon.string.to_int: out of range` / `myon.string.to_float: out of range` の
  `error` を返すようにした。

---

### A-6. 🟡 float の文字列化が `%g`（6桁）で情報を失い、往復不能

- **場所**: `src/value.c` `value_to_cstr` の `TYPE_FLOAT`（L219, `snprintf(buf, ..., "%g", ...)`）
- **内容**: `%g` は既定で有効数字6桁。`myon.print(float)` や
  `myon.string.from_float` は元の `double` を正確に表現できません。
- **影響**: 桁落ち・指数化で**表示 → 再パースの往復ができない**。数値出力の実用性を損なう。
- **再現**:
  ```
  myon.print(123456789.0)          # → 1.23457e+08
  myon.print(3.141592653589793)    # → 3.14159
  myon.print(9007199254740993.0)   # → 9.0072e+15
  ```
  ```
  myon.string.from_float(3.141592653589793)  # → "3.14159"
  ```
- **修正案**: 最短往復表現に切り替える。簡易には `%.17g`、望ましくは
  「`%.15g` → 往復チェック → 必要なら `%.16g`/`%.17g`」のロジック、または
  Grisu/Ryū 系。小数点や指数を含まない場合は `.0` を付ける等の整形も検討。
- **✅ 修正済み**: `src/value.c` に `format_float()` を追加。`%.15g`→`%.16g`→`%.17g`
  の順に試し、`strtod` で元の `double` に一致する最短表現を採用。小数点/指数を
  含まない有限値には `.0` を補い、`nan`/`inf` も明示化。tree-walk と MVM は共通の
  `value_to_cstr()` を通るため両経路で有効。既存 2 テスト（`step16_stdlib`・
  `p_ffi_basic`）の期待出力を `4→4.0` / `1024→1024.0` に更新（float の正しい表示）。

---

## B. 追加したほうがよいもの（Add）

コードそのもののバグではないが、上記の欠陥を「作り込みにくく／早期に捕まえる」ための提案。

### B-1. CI（GitHub Actions 等）が存在しない
- `.github/workflows/` が無く、`make test` の自動実行がありません。
- **提案**: push/PR で `make && make test` を回す最小 CI を追加。可能なら
  複数コンパイラ（gcc/clang）でのマトリクスも。

### B-2. Sanitizer ビルドターゲット / Sanitizer 走行の CI ジョブ
- 今回の A-1〜A-3 は ASan/UBSan で**確定的に**再現できました。
- **提案**: `Makefile` に `make asan`（`-fsanitize=address,undefined`）ターゲットを
  追加し、CI で「ASan/UBSan つきで `make test`」を1本走らせる。UB や
  ヒープオーバーフローの再発を自動検出できます。

### B-3. stdlib の数値/サイズ計算に共通のオーバーフロー安全ヘルパー
- A-1〜A-3 は「境界チェックの生の `+` / `*`」が原因。`int_arith` は守られているのに
  stdlib 側が野放しで、防御が不統一です。
- **提案**: `checked_add_size` / `checked_mul_size`（`__builtin_*_overflow` ラッパ）を
  用意し、確保サイズ計算・範囲チェックを全部それ経由にする。`substring`/`slice`/
  `repeat`/`join` を一巡して置換。

### B-4. 境界値・オーバーフローの回帰テストケース
- 現状 121 ケースありますが、上記のような**極端な整数入力**のケースが不足。
- **提案**: `repeat`/`substring`/`slice`/`to_int`/整数リテラルについて、
  「巨大値を渡したら（クラッシュせず）明示エラーが返る」ことを検証する
  `.myon` + `.expected`（もしくは `.err`）ケースを追加。

### B-5. float 出力仕様のドキュメント化 + テスト
- A-6 の修正に合わせ、「float の文字列化は最短往復表現」を仕様書
  （`docs/myon_spec.md`）に明記し、往復テスト（print→to_float で一致）を追加。

---

## C. まとめ

| ID | 深刻度 | 箇所 | 症状 | 動的検証 |
|----|--------|------|------|----------|
| A-1 | 🔴 | `string.repeat`（value 未使用の生 `*`） | ヒープオーバーフロー/SIGSEGV | ASan: heap-buffer-overflow |
| A-2 | 🔴 | `string.substring` L2421/2427 | 符号付き overflow → OOM/exit70 | UBSan + ASan |
| A-3 | 🔴/🟠 | `array.slice` L2842/2848 | 符号付き overflow → 誤「成功」 | UBSan |
| A-4 | 🟠 | `parser` 整数リテラル | ERANGE 無視で黙ってクランプ | 実行確認 |
| A-5 | 🟠 | `string.to_int`/`to_float` | ERANGE 無視で誤変換を成功扱い | 実行確認 |
| A-6 | 🟡 | `value_to_cstr` の `%g` | float 出力の桁落ち・往復不能 | 実行確認 |

- 最優先は **A-1（メモリ破壊）**、次いで **A-2/A-3（UB＋DoS/整合性）**。
- A-1〜A-3 は「`int_arith` は守っているのに stdlib の境界計算だけ生の演算」という
  **一貫性の欠如**が根本原因で、B-3 の共通ヘルパー導入で面的に解消できます。
- **本 PR で A-1〜A-6 をすべて修正済み**。通常ビルドは警告ゼロ・全テスト
  グリーン（60/68/38 passed）、ASan/UBSan ビルドでも各再現ケースが
  クラッシュ・UB なくエラー値／構文エラーを返すことを確認しました。
  B 群（Add）は今回対象外。
