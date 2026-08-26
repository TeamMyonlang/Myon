# Known Issues (既知の不具合・制限事項)

このファイルは 2 部構成です。

- **第 I 部（項目 1〜4）**: MVM の設計上の制限事項。いずれも「黙って誤動作する」のでは
  なく、**明示的なエラー**として検出されます。
- **第 II 部（項目 5〜27）**: コードベース全体（`src/`）とテストコード（`tests/`）の
  レビューで新たに発見された**バグ・欠陥**。こちらは第 I 部と異なり、**明示的なエラーに
  ならず、クラッシュ（SIGSEGV）・メモリ破壊・情報漏洩・サイレントな誤動作**を引き起こす
  ものを含みます。したがって「すべて明示的なエラーとして検出される」という従来の記述は
  **もはや正しくありません**。

第 II 部の各項目は、実際に再現・実測して確認済みです（ASan/LSan 出力・終了コード・
RSS 実測値を添付）。本ファイルは**バグの収集**を目的としており、修正は含みません。

---

# 第 I 部: MVM の設計上の制限事項

## 1. MVM／`.myc` での package module 取り込み未対応（仕様 §6.3）

インストール済みパッケージの module 取り込み（`module <package-module> as ...`）は、
現状ツリーウォーク型インタプリタ（`.myon` 実行）でのみ対応しています。
`--compile`／`--run-mvm`／`.myc` 実行では、MVM に外部／package module linking の
仕組みがまだ無いため、曖昧にフォールバックせず**明示的なエラー**で失敗します。

該当プログラムはツリーウォーク実行（`.myon`）で動かしてください。MVM 側の
module linking は別の設計課題として今後対応予定です。

## 2. MVM での外部モジュール取り込み未対応

`module external.* as ...`（スクリプト相対の `.myon` ファイル取り込み）も同様に
MVM 非対応で、`--compile` 時のコンパイルエラーになります。

## 3. エンジン境界をまたぐ関数値の受け渡し

MVM で生成した関数値を、ツリーウォーク側で実装されたネイティブ関数へ渡して
コールバック実行させることはできません（実行時に明示エラー）。

- 高階ネイティブメソッド：`array.map` / `array.filter` / `array.reduce`
- `myon.ffi.make_callback`

イベントループ・ネットワーク・FFI などの実行時機構は MVM からブリッジ経由で
共有していますが、*関数値そのもの*は表現形式が異なるため往復できません。
該当プログラムは `.myon` のツリーウォーク実行で動かしてください。

## 4. MVM 版 REPL 未対応

対話モード（REPL）は当面ツリーウォーク実行のみ対応です。

---

# 第 II 部: レビューで発見されたバグ・欠陥（未修正）

以下は `src/` および `tests/` の全体レビューと、ASan/UBSan/LSan・実測・ファジングに
よって**再現確認済み**の不具合です。優先度は概ね A > B > C > D の順です。

| # | 分類 | 概要 | 深刻度 |
|---|------|------|--------|
| 5 | A | `interpreter.c:4149` HTTP クライアントでスタックバッファ外読み出し＋**スタック内容のネットワーク送信** | 致命的 |
| 6 | A | `parser.c:518` `module` 宣言でスタックバッファ外**書き込み** | 致命的 |
| 7 | A | `parser.c:344` 長い `myon.*.*` 識別子でスタックバッファ外**書き込み** | 致命的 |
| 8 | A | 循環参照 struct の出力で SIGSEGV（両エンジン） | 高 |
| 9 | A | `MYON_MAX_CALL_DEPTH=4000` が実スタックに対して過大 → SIGSEGV | 高 |
| 10 | A | async コルーチンのスタック 256KB が呼び出し深度予算に対して不足 → SIGSEGV | 高 |
| 11 | B | 完了済み Task が解放されない（メモリ増大＋O(n²)） | 高 |
| 12 | B | 参照カウントのみでサイクルコレクタ不在 → 循環参照リーク | 中 |
| 13 | B | `spawn_async_task` の `AsyncCtx` が正常終了パスでリーク | 中 |
| 14 | C | 文字列のエスケープシーケンスが**一切デコードされない** | 高 |
| 15 | C | `myon.file` / `myon.array` / `myon.map` が module 名として拒否される（仕様 §10.2 と矛盾） | 中 |
| 16 | C | `module` 宣言が強制されない（宣言なしで標準ライブラリが使える） | 中 |
| 17 | C | `myon.string.upper/lower` が UTF-8 を破壊 | 中 |
| 18 | C | Map が逆順の線形リスト → O(n) 探索・挿入が二次、`keys()` が逆順 | 中 |
| 19 | C | Lexer のエラーメッセージが `%c` でマルチバイト文字を文字化け | 低 |
| 20 | C | MVM とツリーウォークでランタイムエラーの接頭辞が不一致 | 低 |
| 21 | D | `check_output` が終了ステータスを破棄 → **SIGSEGV が PASS になる** | 高 |
| 22 | D | `check_error` が終了コード 139（SIGSEGV）を「期待どおり」と判定 | 高 |
| 23 | D | `make test-asan` が `detect_leaks=0` → リーク 10 件が CI で不可視 | 高 |
| 24 | D | `is_unsupported()` が副作用のあるケースを 2〜3 回実行 | 中 |
| 25 | D | `EXCLUDED_KNOWN` が陳腐化（宣言 22 件 / 実際 3 件） | 低 |
| 26 | D | `.err` 判定がエラー発生フェーズを無視（コンパイルエラーと実行時エラーを同一視） | 中 |
| 27 | D | フィクスチャ無しケース（`??`）が pass/fail どちらにも数えられない | 低 |

---

## A. メモリ安全性・クラッシュ

### 5. `src/interpreter.c:4149` — HTTP クライアント要求ヘッダでスタックバッファ外読み出し（情報漏洩）

`http_client_request()` は 1024 バイトのスタックバッファに要求ヘッダを構築し、
`snprintf` の**戻り値をそのまま送信長として使用**しています。`snprintf` は「切り詰めが
無ければ書き込まれたはずの長さ」を返すため、ヘッダが 1024 バイトを超えると `hn >
sizeof(head)` となり、バッファ境界を越えた**隣接スタックメモリがそのままソケットへ
送信**されます。

```c
char head[1024];
int hn;
if (blen)
    hn = snprintf(head, sizeof(head), "%s %s HTTP/1.0\r\nHost: %s\r\n...", ...);
else
    hn = snprintf(head, sizeof(head), "%s %s HTTP/1.0\r\nHost: %s\r\n...", ...);
if (tls) tls_send_all(it, tls, fd, head, (size_t)hn);   /* hn > sizeof(head) 可能 */
else     http_send_all(it, st, sock, head, (size_t)hn); /* 境界外読み出し */
```

**再現**: ローカルに検査用サーバを立て、パス長 2000 文字程度の URL へ `myon.http.get`。

```
==6106==ERROR: AddressSanitizer: stack-buffer-overflow ... READ of size 2054
    #1 net_send src/net.c:328
    #2 http_send_all src/interpreter.c:3690
    #3 http_client_request src/interpreter.c:4161
Address ... is located in stack of thread T0 at offset 1312 in frame
    #0 http_client_request src/interpreter.c:4096
```

サーバ側の実測は `REQ_BYTES= 2054`。すなわち**1030 バイトの隣接スタックメモリが
ネットワーク越しに送出**されました。単なるクラッシュではなく機密情報漏洩の経路です。

**注**: これは**リグレッション**です。同じ `snprintf` 危険パターンに対する防御
（コード内で "C-2 fix" と呼ばれるもの）は `src/http.c:198` と `src/pkg_fetch.c:269`
には入っていますが、`src/interpreter.c` では抜けています。

### 6. `src/parser.c:518` — `module` 宣言でスタックバッファ外書き込み

`parse_module()` は 256 バイトのバッファに対して `snprintf` の戻り値を `len` に
**加算し続け**、`len < sizeof(buf)` の検査をしません。`sizeof(buf) - len` が
size_t のラップにより巨大値となり、`buf + len` がバッファ外を指した状態で書き込みます。

```c
char buf[256];
size_t len = 0;
len += (size_t)snprintf(buf + len, sizeof(buf) - len, "%s", head->lexeme);
while (match(p, TOK_DOT)) {
    const Token *seg = expect(p, TOK_IDENT, "expected identifier in module path");
    len += (size_t)snprintf(buf + len, sizeof(buf) - len, ".%s", seg->lexeme);
}
```

**再現**: `module s000.s001. ... .s079`（80 セグメント）を含むソース 1 行。

```
==6148==ERROR: AddressSanitizer: stack-buffer-overflow ... WRITE of size 6
    #2 parse_module src/parser.c:518
  [32, 288) 'buf' (line 505) <== Memory access at offset 291 overflows this variable
```

**深刻な点**: これは**パース時**に起きます。`--compile` のみ（プログラム実行なし）で
再現するため、「信頼できないソースを構文チェックする」だけでメモリ破壊が可能です。

### 7. `src/parser.c:344` — 長い `myon.*` 修飾識別子でスタックバッファ外書き込み

`parse_primary()` の 128 バイトバッファが #6 と同一のパターンです。

```c
char buf[128];
size_t len = (size_t)snprintf(buf, sizeof(buf), "myon");
while (check(p, TOK_DOT) && seg_is_name(peek_type(p, 1))) {
    advance(p);
    const Token *seg = advance(p);
    len += (size_t)snprintf(buf + len, sizeof(buf) - len, ".%s", seg->lexeme);
}
```

```
==6158==ERROR: AddressSanitizer: stack-buffer-overflow ... WRITE of size 6
    #2 parse_primary src/parser.c:344
```

**参考**: `src/types.c:125` と `src/pkg_ops.c:303` は同種の累積に対して
`len < sizeof(...)` を検査しています。`parser.c` の 2 箇所のみ防御が抜けています。

### 8. 循環参照 struct の出力で SIGSEGV（ツリーウォーク・MVM 両方）

`src/value.c` の `value_to_cstr()` は `TYPE_STRUCT` / `TYPE_ARRAY` / `TYPE_MAP` へ
再帰しますが、**深度制限も訪問済み集合も持ちません**。自己参照する struct を
`print` するとスタックを食い尽くします。

```c
case TYPE_STRUCT: {
    StructData *st = &v->as.obj->as.st;
    char *s = myon_strdup(st->type_name);
    ...
    char *fv = value_to_cstr(&st->field_vals[i]);   /* サイクルで無限再帰 */
```

**再現**: 自身を参照するフィールドを持つ struct を作り `print` する。
素の実行で終了コード **139 (SIGSEGV)**、ASan では
`==10344==ERROR: AddressSanitizer: stack-overflow`。両エンジンで同一。

### 9. `MYON_MAX_CALL_DEPTH = 4000` が実スタックサイズに対して過大

`src/interpreter.c:154` の再帰上限は 4000 フレームですが、1 フレームの実スタック消費が
大きく、上限に到達する前に OS のスタックが枯渇します。

**再現**:

| 条件 | 結果 |
|------|------|
| `ulimit -s 1024` | 終了コード **139 (SIGSEGV)** |
| `ulimit -s 2048` | 終了コード **139 (SIGSEGV)** |
| ASan ビルド | 終了コード **134 (SIGABRT)** = ASan stack-overflow |

```
==4511==ERROR: AddressSanitizer: stack-overflow on address 0x7ffcc6542cf8
    #1 find_local src/env.c:47
    #2 env_get src/env.c:70
    #7 call_function src/interpreter.c:3439
```

これは `tests/cases/p52_recursion_limit` が本来検証したい「上限に達したら**明示的な
エラー**で止まる」という目的を、そのまま無効化しています（→ 項目 22 も参照。この
クラッシュはテストハーネス上「ok」と表示されます）。

### 10. async コルーチンのスタック（256KB）が呼び出し深度予算に対して不足

`src/event_loop.c` の `TASK_STACK_SIZE` は `256 * 1024` バイト固定です。一方
`async_task_entry()`（`src/interpreter.c:3510` 付近）は `it->call_depth = 0` を
代入するため、**コルーチンは 256KB のスタックで 4000 フレーム分の予算**を与えられます。

**再現**: async 関数内で再帰。

| 再帰深度 | 結果 |
|----------|------|
| 400 | 正常終了 |
| 800 | 終了コード **139 (SIGSEGV)** |

つまり深度上限チェックは async 経路では実質機能していません。

---

## B. リソース枯渇・メモリリーク

### 11. 完了した Task が回収されない（メモリ増大 + O(n²)）

`src/event_loop.c` の `event_loop_spawn()` は Task をリストの末尾へ**線形探索で**追加し、
**完了した Task を取り除くコードが存在しません**。解放は `event_loop_destroy()`
（プロセス終了時）のみです。

```c
Task *event_loop_spawn(EventLoop *loop, void (*entry)(void *ud), void *ud) {
    Task *t = (Task *)calloc(1, sizeof(Task));
    t->stack_size = TASK_STACK_SIZE;             /* 256 * 1024 */
    t->stack = malloc(t->stack_size);
    ...
    if (!loop->tasks) { loop->tasks = t; }
    else { Task *p = loop->tasks; while (p->next) p = p->next; p->next = t; }  /* O(n) */
    return t;
}
```

さらにスケジューラ側のヘルパ（`event_loop_run_once` など約 10 箇所）もすべてリスト全体を
走査するため、**生成した総タスク数に対して O(n²)** になります。

**実測（`spawn` して即完了する Task を N 個生成）**:

| N | 最大 RSS | 実時間 |
|---|----------|--------|
| 100 | 10.9 MB | — |
| 1,000 | 17.3 MB | — |
| 4,000 | 55.5 MB | — |
| 10,000 | **132.2 MB** | **3.35 s** |

長時間動作するサーバ（`p5_http_serve_static` のような用途）では接続ごとに Task を
spawn するため、実質的なメモリリークとして働きます。

### 12. 参照カウントのみでサイクルコレクタが無い → 循環参照リーク

`src/value.c` の `value_free()` は `--refcount` のみで、循環検出を行いません。
仕様どおりの参照カウント実装ですが、サイクルは永久にリークします。

**実測**: 自己参照 struct 50 個で **5,300 バイト / 250 アロケーション**のリーク
（LSan 検出）。項目 8 と同じデータ構造で、片方はクラッシュ、片方はリークになります。

### 13. `spawn_async_task` の `AsyncCtx` が正常終了パスでリーク

`src/interpreter.c:3548` の `myon_xmalloc(sizeof(AsyncCtx))` が、完了しない（あるいは
回収されない）タスクについて解放されません。

**実測**: `tests/cases/p5_http_serve_static` の**成功パス**で 56 バイトのリーク。
LSan を有効化して全ケースを実行すると、**10 ケース**でリークが検出されます
（→ 項目 23。CI ではこれが `detect_leaks=0` により完全に隠蔽されています）。

---

## C. 機能・仕様適合

### 14. 文字列リテラルのエスケープシーケンスが一切デコードされない

`src/interpreter.c:577` の `interpolate_string()` は `{expr}` 補間（仕様どおり）を
実装していますが、**バックスラッシュエスケープの処理がコードベースのどこにも存在
しません**。

**再現**: `"\r"` を出力すると `0x0d` ではなく **`5c 72`**（`\` と `r` の 2 バイト）が
出ます。`\n` `\t` `\\` `\"` も同様。

**影響**:
- 同梱の `examples/cli_progress.myon:49` が `\r` によるカーソル復帰を前提にしており、
  **サンプルが意図どおり動作しません**。
- `README.md:22` と `docs/myon_spec.md:503` の記述と**矛盾**します。

エラーにならず**サイレントに誤った出力**を生む点で、第 I 部の「明示的なエラー」原則の
反例です。

### 15. `myon.file` / `myon.array` / `myon.map` が module 名として拒否される

`src/interpreter.c:5049` の `BUILTIN_MODULES[]` は次のとおりです。

```c
"myon.stdio", "myon.math", "myon.string", "myon.ffi",
"myon.time", "myon.random", "myon.net", "myon.http", NULL
```

`myon.file` / `myon.array` / `myon.map` が**欠落**しており、`module myon.file` と
書くとエラーになります。一方で仕様 §10.2 は「`module myon.stdio` によりファイル I/O が
有効になる」と述べており、モジュール分割の記述と実装が一致していません。

### 16. `module` 宣言が強制されない

`module` 宣言を**一切書かなくても**標準ライブラリの関数（`print` を含む）が呼び出せます。
宣言はパースされますが、機能のゲートとして機能していません。項目 15 と合わせて、
module システムが仕様と実装の両面で未完成です。

### 17. `myon.string.upper` / `lower` が UTF-8 文字列を破壊

`src/interpreter.c:2641` はバイト単位で `toupper()` / `tolower()` を適用します。

**再現**: `myon.string.upper("café")` → **`"CAFé"`**（マルチバイト部分が変換されない、
ロケール次第では不正バイト列を生成しうる）。

### 18. Map が逆順の線形リストとして実装されている

Map の探索が O(n)、挿入が全体で二次になります。また `keys()` が**挿入順の逆**を返します。

**再現**:
- `a`, `b`, `c` の順に挿入 → `keys()` は **`[c, b, a]`**。
- N = 8,000 件の挿入で **0.162 s**（線形なら数 ms 程度）。

順序についての仕様上の保証が明示されていないため「バグ」か「仕様」か曖昧ですが、
挿入順でも辞書順でもない順序は利用者の期待を裏切ります。

### 19. Lexer のエラーメッセージがマルチバイト文字を文字化けさせる

`src/lexer.c:337` 付近の "unexpected character" メッセージが `%c` で 1 バイトのみを
出力するため、UTF-8 文字が壊れて表示されます（診断メッセージの品質問題）。

### 20. MVM とツリーウォークでランタイムエラーの接頭辞が不一致

- ツリーウォーク: `myon: runtime error at line N:`
- MVM (`src/mvm_vm.c:174` 付近 `vm_error`): `runtime error (line N):`

「両エンジンの出力一致」という設計目標（`tests/run_mvm_tests.sh` が担保するはずのもの）に
反します。エラー出力は `.err` フィクスチャで内容比較されないため（→ 項目 26）
テストをすり抜けています。

---

## D. テストハーネスの欠陥

このカテゴリは「バグを検出できない」という点で、A〜C の問題が長期間放置される
根本原因になっています。

### 21. `tests/run_tests.sh` の `check_output` が終了ステータスを破棄する

```bash
got=$("$MYON" "$src" 2>/dev/null | strip_cr)      # 終了ステータスを捨てている（100 行目）
expected_txt=$(strip_cr < "$expected")
if [ "$got" == "$expected_txt" ]; then echo "  ok   $name"; pass=$((pass+1))
```

**実証**: `"A"` を出力・フラッシュした直後に SIGSEGV で死ぬ（終了コード 139）ケースを
作成したところ、ハーネスは **`ok`（PASS）** と報告しました。つまり期待出力を出し切って
からクラッシュするプログラムは、テストスイート上「成功」になります。

### 22. `check_error` が終了コード 139（SIGSEGV）を「期待どおり」と扱う

```bash
"$MYON" "$src" >/dev/null 2>&1
if [ $? -ne 0 ]; then echo "  ok   $name (errored as expected)"
```

「非ゼロなら合格」なので、**SIGSEGV (139) / SIGABRT (134) も合格**になります。
`make test-asan` の実際の出力には次の行が含まれますが、`ok` として集計されています。

```
p52_recursion_limit (both errored: tree-walk=134, MVM=1)
```

134 は ASan のスタックオーバーフロー検出による SIGABRT であり、項目 9 の証跡そのものです。

### 23. `make test-asan` がリーク検出を無効化している

`Makefile:178`:

```make
export ASAN_OPTIONS="abort_on_error=1:halt_on_error=1:detect_leaks=0"; \
```

`detect_leaks=0` のため、項目 11〜13 を含む**リーク 10 ケースが CI で完全に不可視**です。
サニタイザ用ターゲットを用意しているのにリーク検出だけ切っており、意図的なのか
暫定回避なのかコメントもありません。

### 24. `is_unsupported()` が副作用のあるケースを 2〜3 回実行する

`tests/run_mvm_tests.sh` の `is_unsupported()` は判定のために `--compile` と
`--run-mvm` を**先に実行**し、その後で本番の実行が走ります。結果として同一ケースが
2〜3 回実行され、ポート 19099 への bind や `/tmp` への書き込みといった副作用が
多重に発生します（フレーク・ポート競合の温床）。

### 25. `EXCLUDED_KNOWN` が陳腐化している

`EXCLUDED_KNOWN` には 22 個の名前が宣言されていますが、実際の実行で除外されるのは
**3 件**のみです。また実際に除外されている `p_module_alias` はリストに**0 回**しか
現れません（＝未文書化の除外）。除外リストが実態を反映しておらず、
「何が未対応か」の情報源として信頼できません。

### 26. `.err` 判定がエラー発生フェーズを区別しない

`run_mvm_tests.sh` の `.err` 分岐は「両エンジンとも非ゼロ終了」のみを確認します。

**実例**: `tests/cases/step8_shadow` は
- ツリーウォーク: 終了コード **1**（実行時エラー）
- MVM: 終了コード **65**（コンパイルエラー）

とフェーズが異なりますが `ok` と報告されます。エラーメッセージ本文も比較されないため、
項目 20 の接頭辞不一致もここをすり抜けます。

### 27. フィクスチャ無しケースが pass/fail どちらにも数えられない

`tests/run_tests.sh:143`（および `run_mvm_tests.sh:153`）:

```bash
echo "  ??   $name (no .out or .err fixture)"
```

`pass` も `fail` もインクリメントしないため、フィクスチャを付け忘れたケースは
**静かに無視**され、合計件数からも消えます。

---

## 付録: 確認したが問題なしだった項目（ネガティブな結果）

同じ調査の中で検査し、**正しく実装されている**ことを確認した項目です。再調査の重複を
避けるために記録します。

- ゼロ除算・ゼロ剰余、および `INT_MIN / -1` は正しくトラップされます。
- 配列の添字境界チェックは正/負ともに機能します。
- FFI の `read_i64` はアロケーション範囲外アクセスを検出します。
- `src/net.c` の `strcpy` 使用箇所はいずれもサイズが保証されています。
- `src/types.c` / `src/pkg_ops.c` の `snprintf` 累積は `len < sizeof(...)` で防御済み
  （＝項目 6/7 が防御漏れであることの裏付け）。
- ランダムトークン列によるパーサのファジング 4,000 回でクラッシュなし
  （項目 6/7 は「長いドット連結」という特定の形が必要で、ランダム生成では出にくい）。
- 文字列補間 `{expr}` は仕様どおり動作します（壊れているのはエスケープのみ＝項目 14）。

---

> **注意（コード内コメントの番号について）**：`src/` の一部コメントには
> `known-issue #5` や `known-issue.md #6` のような番号参照が残っています。
> これらは**過去のバージョンの本ファイルの項番**であり、上記の番号とは
> 対応しません（該当項目はいずれも修正済みです。例: #1 = `longjmp` リーク、
> #5 = Windows `SOCKET` の切り詰め、#6 = 暗号学的に安全な乱数の不在
> → `myon.random.secure_int` を追加）。歴史的経緯の記録として残しています。
