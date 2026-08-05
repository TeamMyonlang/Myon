# Myon

半分ネタ、半分実用(？)

**Myon** は型に厳密なプログラミング言語です。言語仕様は [`docs/myon_spec.md`](docs/myon_spec.md) を参照してください。

## 実装状況

`make test` により、下表の各ステップ・項目に対応する回帰テスト（計 30 ケース）がすべてパスします。

### Step 0〜18（コア実装）

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 0 | プロジェクト準備（Makefile・構成・LICENSE） | ✅ |
| Step 1 | 字句解析器（Lexer） | ✅ |
| Step 2 | AST 定義 | ✅ |
| Step 3 | パーサー（変数代入・`myon.print`・式の優先順位） | ✅ |
| Step 4 | 型システムの基礎（型チェック・キャスト・`myon.nil`） | ✅ |
| Step 5 | 制御構文（if/elif/else・while・for・break/continue） | ✅ |
| Step 6 | 関数（`myon.func`・複数戻り値・`error`・`myon.lambda`） | ✅ |
| Step 7 | 配列（`myon.array`・push/pop/length・インデックスアクセス） | ✅ |
| Step 8 | スコープ規則と `myon.expose`（ブロックスコープ・シャドーイング禁止） | ✅ |
| Step 9 | 構造体（`myon.struct`・メソッド・`self`・`myon.extends`） | ✅ |
| Step 10 | 文字列補間（`"... {式} ..."`） | ✅ |
| Step 11 | モジュールシステム（`module myon.stdio` などの解決） | ✅ |
| Step 12 | 整数オーバーフローチェック（実行時エラー） | ✅ |
| Step 13 | 拡張リテラル（16進/8進/指数表記）と辞書型（`myon.map`） | ✅ |
| Step 14 | 型推論（単一リテラル代入に限り型注釈省略を許可） | ✅ |
| Step 15 | ジェネリクス（`myon.struct Box<T>` / `myon.func f<T>`） | ✅ |
| Step 16 | 標準ライブラリ（`myon.math` / `myon.string`） | ✅ |
| Step 17 | async/await（実験的・疑似非同期 → Phase5 で協調的イベントループへ本格化） | ✅ |
| Step 18 | 総合テスト（回帰テストとして `tests/cases/` に集約） | ✅ |

### Phase 2（追加実装 P1〜P7）

| 項目 | 内容 | 状態 |
|---|---|---|
| P1 | `()`/`[]` 内の改行を文区切りにしない（複数行の引数リスト） | ✅ 実装 |
| P2 | `myon.not` の前置専用ルールをドキュメントに明記 | ✅ ドキュメント |
| P3 | `myon` 単体起動時の対話式実行（REPL） | ✅ 実装 |
| P4 | ファイル I/O（`myon.file.read`/`write`/`append`/`exists`） | ✅ 実装 |
| P5 | エラーメッセージの詳細化（列番号・ソース抜粋・`^` マーカー） | ✅ 実装 |
| P6 | `myon.async`/`myon.await` の実行モデル → 当初は疑似非同期に確定、Phase5 で協調的イベントループへ本格化（OSスレッド化は引き続き不採用） | ✅ 設計確定 |
| P7 | ジェネリクスの型制約 → 導入しないことに確定 | ✅ 設計確定 |

P1・P3・P4・P5 には対応するテストケース（`tests/cases/p1_multiline_args`,
`tests/cases/p4_file_io`, `tests/cases/p5_error_detail` ほか）を追加しています。
P2・P6・P7 は仕様書（`docs/myon_spec.md`）の更新で完了です。

### Phase 3（C FFI — 外部共有ライブラリ呼び出し）

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1 | プラットフォーム抽象化層（`dlopen`/`dlsym`/`dlclose` ラップ、`src/ffi_platform.{h,c}`） | ✅ 実装 |
| Step 2 | FFI 型・ハンドル管理レイヤ（`src/ffi.{h,c}`） | ✅ 実装 |
| Step 3 | 呼び出しテーブル（libffi 不使用、引数パターン別 C ラッパー、`src/ffi_call.{h,c}`） | ✅ 実装 |
| Step 4 | Myon 言語結合（`module myon.ffi`：`load`/`close`/`call_i`/`call_d`/`call_p`/`call_v`） | ✅ 実装 |
| Step 5 | 回帰テスト・ドキュメント仕上げ | ✅ 実装 |

### Phase 3.1（FFI 拡張 — メモリ確保・文字列デリファレンス・バッファ引数）

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1 | メモリ確保・解放プリミティブ（`myon.ffi.alloc` / `myon.ffi.free`、`src/ffi.{h,c}`） | ✅ 実装 |
| Step 2 | sig シグネチャへの `'b'`（バッファ／ブロックID引数）追加、out 引数対応 | ✅ 実装 |
| Step 3 | 文字列デリファレンス（`myon.ffi.read_cstr`、`ffi_read_cstring`） | ✅ 実装 |
| Step 4 | バイト列読み書き（`myon.ffi.write_bytes` / `read_bytes`）＋ `read_i64` ヘルパー | ✅ 実装 |
| Step 5 | 安全性の見直しとドキュメント整備 | ✅ 実装 |
| Step 6 | 実地検証（`sqlite3_open`）とテスト追加 | ✅ 実装 |

Phase 3.1 では、Myon 側から C に渡すための生メモリ領域を確保・解放できる
`myon.ffi.alloc` / `myon.ffi.free`、確保したブロックを sig `'b'` で C 関数へ渡す
仕組み（`sqlite3_open` のような「出力引数」対応）、`const char*` の戻り値を Myon の
`str` に読み出す `myon.ffi.read_cstr`、確保ブロックへのバイト列読み書き
`myon.ffi.write_bytes` / `myon.ffi.read_bytes`、および出力引数として書き込まれた
ポインタ値を取り出す `myon.ffi.read_i64`（リトルエンディアン int64）を追加しました。
回帰テストは環境非依存の `libz` を用い、文字列デリファレンス（`tests/cases/p31_ffi_read_cstr`）、
バイト列書き込み＋`crc32`（`tests/cases/p31_ffi_bytes_crc32`）、`read_i64`
（`tests/cases/p31_ffi_read_i64`）、純粋なメモリ管理（`tests/cases/p31_ffi_alloc_free`）
をカバーします。文字列を返す C 関数の例は `examples/ffi_zlib_version.myon`、出力引数
（`sqlite3_open`）の例は `examples/ffi_sqlite_open.myon` にあります（後者は `libsqlite3` が
必要なため `examples/` のみ）。

> str型はNUL終端の `char*` として表現されるため、`write_bytes` / `read_bytes` は
> NULバイトを含むデータでは途切れる（バイナリセーフではない）という既知の制約が
> あります。詳細は仕様書 10.3.1 節を参照してください。

`module myon.ffi` を宣言すると、ビルド済みの共有ライブラリ（Linux の `.so` など）に
含まれる C 関数を実行時に呼び出せます。対応する値の型は `int` / `float` / ポインタ（`int`
として表現）/ `str` の4種類で、構造体の値渡し・値返しは対象外です。対応 OS は Linux
（macOS / Windows は `myon.ffi.load` 時に「未対応」の `error` を返すスタブ）。詳細と制約は
仕様書 [`docs/myon_spec.md`](docs/myon_spec.md) の「10.3 C FFI」節を参照してください。
テストは環境非依存の `libm`（数学ライブラリ）を用います（`tests/cases/p_ffi_basic`,
`tests/cases/p_ffi_load_fail`, `tests/cases/p_ffi_close`）。SDL2 を使った非対話デモは
`examples/ffi_sdl_window.myon` に置いてあります（回帰テストには含めません）。

### Phase 3.5（`myon.math` / `myon.string` 拡張）

Step 16 で最小実装した標準ライブラリ `myon.math` / `myon.string` を、実用的な
ラインナップへ拡張・修正しました。

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1 | 数値型保持ルールの統一（`max`/`min`/`floor`/`ceil` を int/float パスへ分岐、`both_int` ヘルパー、2^53超の int64 を正確に扱う） | ✅ 実装 |
| Step 2 | `myon.string.length` を UTF-8 文字数（コードポイント数）へ修正、`byte_length` を新設（`utf8_char_count`） | ✅ 実装 |
| Step 3 | `myon.math` フルセット（三角/逆三角/`atan2`・`log`/`log2`/`log10`/`exp`・`round`/`trunc`/`mod`/`sign`/`clamp`・`pi`/`e`） | ✅ 実装 |
| Step 4 | `myon.string` フルセット（`substring`/`split`/`join`/`trim`/`replace`/`index_of`/`starts_with`/`ends_with`/`repeat`/`to_int`/`to_float`/`from_int`/`from_float`、`utf8_byte_offset`） | ✅ 実装 |
| Step 5 | 回帰テスト・ドキュメント整備 | ✅ 実装 |

数値関数は「全引数が int なら int、1つでも float なら float」という暗黙昇格ルールに
統一しました。整数のまま意味が保たれる関数（`abs`/`max`/`min`/`floor`/`ceil`/`round`/
`trunc`/`mod`/`sign`/`clamp`）は `double` を経由せず、`2^53` を超える `int64` 値でも
丸め誤差なく扱えます。文字列関数の `length` は **文字数**、`byte_length` は **バイト数**
を返し、`substring` / `index_of` など索引系は全て **文字数ベース**（UTF-8 マルチバイト
安全）です。`mod` / `substring` / `repeat` / `to_int` / `to_float` はゼロ除算・範囲外・
パース失敗を `(value, error)` の2値で返します。回帰テストは
`tests/cases/p35_math_int_precision`（境界値精度）、`p35_math_full`（数学フルセット）、
`p35_string_utf8_length`（文字数/バイト数）、`p35_string_full`（文字列フルセット）で
カバーします。全関数のシグネチャは仕様書
[`docs/myon_spec.md`](docs/myon_spec.md) の「10.4 標準ライブラリ myon.math /
myon.string（Phase3.5）」節にまとめてあります。

### Phase 4（`myon.array` / `myon.map` メソッド拡充、`myon.math` 整数演算、`myon.time` / `myon.random` 新設）

コレクション型のメソッドを実用的なラインナップへ拡充し、大きな整数の冪乗を正確に
計算する整数演算と、時刻取得・乱数生成の新規モジュールを追加しました。

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1 | `myon.array` に `sort` / `sort_desc` / `reverse` / `contains` / `index_of` / `slice` を追加（`sort`系は破壊的、`qsort`＋型判定`compar`、`slice`は範囲外を`error`） | ✅ 実装 |
| Step 2 | `myon.map` に `keys` / `values` / `length` を追加（`MapEntry`連結リストを走査、順序保証なし） | ✅ 実装 |
| Step 3 | `myon.array` に高階関数 `map` / `filter` / `reduce` を追加（`myon.lambda`対応、既存`call_function`を再利用） | ✅ 実装 |
| Step 4 | `myon.math` に整数演算 `gcd` / `lcm` / `pow_int` を追加（繰り返し二乗法・オーバーフロー検出、`double`非経由で`2^62`も正確） | ✅ 実装 |
| Step 5 | 新規モジュール `myon.time`（`now` / `now_ms` / `sleep_ms`） | ✅ 実装 |
| Step 6 | 新規モジュール `myon.random`（`seed` / `int` / `float`、初回自動シード、`rand()`ベース） | ✅ 実装 |
| Step 7 | ドキュメント整備・回帰テスト追加 | ✅ 実装 |

`myon.array` は `sort` / `sort_desc` / `reverse`（破壊的）、`contains` / `index_of` /
`slice`（非破壊）、および `map` / `filter` / `reduce`（`myon.lambda` を取る高階関数）を
備えます。`myon.map` は `keys` / `values` / `length` で中身を列挙できます（順序は保証
しないため、必要なら返り配列を `sort()` してから使います）。`myon.math.pow_int` は
`double` を経由しないため `2^62`（`4611686018427387904`）のような大きな整数冪も桁落ち
なく計算でき、負指数・オーバーフローは `error` を返します。`myon.time` は UNIX エポック
秒/ミリ秒の取得とスリープ、`myon.random` は `srand`/`rand` を薄くラップした乱数生成を
提供します（**暗号学的に安全ではありません**）。回帰テストは
`tests/cases/p4_array_methods`（sort/reverse/contains/index_of/slice）、
`p4_array_higher_order`（map/filter/reduce）、`p4_map_methods`（keys/values/length、
順序非依存）、`p4_math_integer`（gcd/lcm/pow_int）、`p4_time_basic`・`p4_random_basic`
（時刻・乱数は不変条件のみ検証し `.out` を決定的化）でカバーします。全シグネチャは
仕様書 [`docs/myon_spec.md`](docs/myon_spec.md) の「7.1 配列メソッド」「14.2 辞書/マップ型」
「10.4 myon.math」「10.5 myon.time」「10.6 myon.random」各節を参照してください。

### Phase 4.1（GUI/ゲーム制作向け FFI 拡張 — 型付き書き込み・構造体レイアウト DSL・配列一括読み書き・コールバック）

SDL/OpenGL のような GUI・ゲームライブラリを実用的に FFI から叩けるよう、Phase3.1
までの「単純な値渡し＋バイト列」を4系統で拡張しました。いずれも C 側のヘッダは
一切読まず、Myon 側が申告したレイアウト・型をそのまま信じる方針（Phase3 以来）です。

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1 | 型付きメモリ書き込み `myon.ffi.write_i64`/`write_i32`/`write_f64`/`write_f32`（NULバイトを含む値も欠落なく書ける、`ffi_mem_write_*`） | ✅ 実装 |
| Step 2 | 構造体レイアウト DSL `myon.ffi.struct_def`/`struct_alloc`/`struct_write`/`struct_read`（自然アライメント・末尾パディング自動計算、`ffi_struct_define`ほか） | ✅ 実装 |
| Step 3 | 配列一括読み書き `myon.ffi.write_array_*`/`read_array_*`（i32/i64/f32/f64、`ffi_mem_write_array_*`/`ffi_mem_read_array_*`） | ✅ 実装 |
| Step 4 | コールバック関数ポインタ（限定版）`myon.ffi.make_callback`/`free_callback`（libffi 不使用、静的トランポリン、`src/ffi_callback.{h,c}`） | ✅ 実装 |
| Step 5 | 仕様書 10.3.2 節を追加 | ✅ 実装 |
| Step 6 | 回帰テスト追加（`p41_*`、コールバックは fixture の `.so` を事前ビルド） | ✅ 実装 |
| Step 7 | README 更新・`SDL_Rect` サンプル追加 | ✅ 実装 |

型付き書き込みは値の生バイト列をリトルエンディアンで直接書き込むため、`int` の
エンコードに NUL バイトが混ざっても途切れません（`write_bytes` の制約を解消）。
構造体レイアウト DSL は `["i32","i32","i32","i32"]` のような型リストから各フィールドの
オフセットと合計サイズを自動計算し、C のデフォルト構造体パディング（自然アライメント＋
末尾パディング）に従います（`SDL_Rect` はパディングなしで16バイト、`i32`/`i64` 混在は
自動でパディング）。`struct_read` の戻り値スカラー型はフィールド型から実行時に決まります
（Myon のタプル返却が型なし配列パックのため単一関数で自然に扱える、という設計判断）。
配列一括読み書きは頂点/色配列のような同型データを1回の呼び出しでやり取りします。
コールバックは **引数最大4個・int64/ptr のみ・戻り値 int64 のみ・同時16スロット** の
限定スコープで、libffi の closure を使わず静的トランポリン関数群を介して Myon 関数を
呼び返します（シングルスレッド前提）。

回帰テストは `tests/cases/p41_ffi_write_typed`（型付き書き込みの roundtrip、NUL バイトを
含む値を明示）、`p41_ffi_struct_dsl`（i32 のみ／i32・i64 混在パディング）、
`p41_ffi_array_bulk`（配列 roundtrip＋範囲外エラー）、`p41_ffi_callback`（C→Myon コール
バック、スロット再利用）でカバーします。コールバックテストは CI 再現性のため
`tests/fixtures/ffi_callback_test.c` を `tests/run_tests.sh` が実行前に `.so` へビルド
します。構造体 DSL を使った `SDL_Rect` の組み立て例は `examples/ffi_sdl_rect.myon` に
あり、SDL2 が無い環境でも構造体の組み立て・確認部分は独立して動作します。全シグネチャ
と設計判断は仕様書 [`docs/myon_spec.md`](docs/myon_spec.md) の「10.3.2 GUI/ゲーム制作
向け FFI 拡張（Phase4.1）」節を参照してください。

### Phase 5（async/await 本格化 — 協調的イベントループ・`myon.net`・`myon.http`）

Step17 の「疑似非同期（呼び出し即同期実行）」を廃止し、シングルスレッドの
**協調的イベントループ**（`ucontext` ベースのコルーチン、`src/event_loop.{h,c}`）へ
置き換えました。あわせて低水準ソケット `myon.net`（TCP/UDP）と、その上に構築した
簡易 HTTP モジュール `myon.http`（静的配信／ルーティングサーバー＋自前 TCP クライアント）
を追加しています。本物の OS スレッド化は Phase2 P6 の判断どおり採用せず（`refcount` の
スレッドセーフ化が必要で侵襲が大きすぎるため）、切り替えは `await`／I/O 待ち地点のみで
起こる協調的マルチタスク（Python asyncio / JS async-await と同じモデル）です。

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1 | イベントループ基盤（`ucontext` コルーチン・`select(2)` 多重化・spawn/await/sleep/IO待ち、`src/event_loop.{h,c}`） | ✅ 実装 |
| Step 2 | インタプリタ統合（`TYPE_TASK`/`OBJ_TASK`、`is_async` 関数が Task 値を返す、`EXPR_AWAIT` の本実装、トップレベルも1タスク化） | ✅ 実装 |
| Step 3 | `myon.time.sleep_ms` の非同期対応（コルーチン内は `event_loop_sleep_ms`、同期文脈はブロッキングにフォールバック） | ✅ 実装 |
| Step 4 | 新規モジュール `myon.net`（`tcp_socket`/`udp_socket`/`bind`/`listen`/`local_port`/`accept`/`connect`/`send`/`recv`/`send_to`/`recv_from`/`close`、ノンブロッキング＋協調待機、`src/net.{h,c}`） | ✅ 実装 |
| Step 5 | 新規モジュール `myon.http`（`serve_static`/`serve`/`get`/`post`、HTTP/1.0・自前 TCP クライアント、`src/http.{h,c}`） | ✅ 実装 |
| Step 6 | 仕様書更新（14.9 実行モデル書き換え・10.7 `myon.net`・10.8 `myon.http`・15 Open Questions） | ✅ 実装 |
| Step 7 | 回帰テスト追加（`p5_async_order`／`p5_net_tcp_echo`／`p5_net_udp_echo`／`p5_http_serve_static`、いずれも自己完結でCI非依存） | ✅ 実装 |
| Step 8 | README 更新・サンプル追加（`http_static_server`／`http_router_server`／`net_game_echo`） | ✅ 実装 |

各ソケットは常にノンブロッキング（`O_NONBLOCK`）で作られ、`myon.async` 関数の中から
呼ぶと would-block 時に自動でイベントループへ制御を譲り、他のタスクへ切り替わります。
`myon.async` を使わないトップレベル（同期文脈）から呼んだ場合は、単一 fd の `select()`
による同期待機にフォールバックするため、後方互換的な単純スクリプトでもそのまま使えます。
`myon.http.serve_static`/`serve` の accept ループはデーモンタスクとしてマークされ、
プログラム終了時のドレイン処理では無視されるため、サーバーを起動しつつ別タスクで
クライアント処理をして終了する自己完結スクリプトがハングせず終了します。

回帰テストは `tests/cases/p5_async_order`（3タスクが sleep 時間の短い順＝spawn 順とは
異なる順で完了することを検証）、`p5_net_tcp_echo` / `p5_net_udp_echo`（同一プロセス内で
サーバー役・クライアント役を両方 `myon.async` タスクとして立て、ポート0＋`local_port`
で衝突を避けつつ1往復のエコーを検証）、`p5_http_serve_static`（`serve_static` を
デーモンタスクとして起動しつつ `myon.http.get` で取得、ステータス・本文を検証）で
カバーします。いずれも外部プロセス（`nc`/`curl`）に依存しない自己完結テストです。
サンプルは `examples/http_static_server.myon`（`python -m http.server` 相当の静的配信）、
`examples/http_router_server.myon`（パスに応じて分岐するルーティング＋404）、
`examples/net_game_echo.myon`（UDP による1対1の座標 echo デモ）に置いてあります
（常駐サーバー／ネットワークI/Oを伴うため回帰テストには含めません）。全シグネチャと
実行モデルの詳細は仕様書 [`docs/myon_spec.md`](docs/myon_spec.md) の「14.9 並行処理・
非同期処理」「10.7 ネットワーク myon.net」「10.8 HTTP myon.http」各節を参照してください。

### Phase 5.1（ネットワーク実用化／ゲーム制作支援／多重代入のスコープ解決バグ修正）

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1〜3 | 多重代入のスコープ解決バグ修正（`x, y = ...` が外側スコープの既存変数を更新せず誤ってローカルへシャドウしていた不具合を修正。単純代入と多重代入で共通ヘルパー `assign_or_define()` を使うようリファクタし、`{ }` ブロック内での多重代入シャドウイングも単純代入同様に禁止。仕様書 9.2 更新・回帰テスト `p51_scope_multi_assign_outer` / `p51_scope_multi_assign_shadow_forbidden` 追加） | ✅ 実装 |
| Step 4〜5 | DNS 名前解決（`src/net.c` の `fill_addr()` を `getaddrinfo()` ベースへ書き換え。IPv4 リテラルは従来どおり `inet_pton()` の高速パスを維持し、ホスト名は `getaddrinfo()` で解決。`myon.net.connect`/`bind`/`send_to` がホスト名を受け付けるようになった。回帰テスト `p51_net_dns_localhost` 追加＝ループバックのみで CI 安全） | ✅ 実装 |
| Step 6 | HTTPS/TLS 対応（新規 `src/tls.{h,c}` に OpenSSL の薄いラッパを実装し、`myon.http.get`/`post` が `https://` URL で TLS ハンドシェイクを行うよう分岐。SNI＋システム既定トラストストアによる証明書検証をベストエフォートで有効化。Makefile に `-lssl -lcrypto` を追加＝OpenSSL 開発パッケージが必須依存に。デモ `examples/http_https_get.myon` 追加） | ✅ 実装 |
| Step 7〜9 | SDL_Event 読み取り支援（out 方向の構造体読み取りの実証）。`myon.ffi.read_i32`（4バイト読み出し、`write_i32` と対称）を追加し、`SDL_PollEvent(SDL_Event *)` で書き戻されたイベント種別（offset 0 の `Uint32 type`）等を読めるように。デモ `examples/ffi_sdl_event_loop.myon`、仕様書に SDL_Event レイアウト・オフセット・既知の制約を追記 | ✅ 実装 |
| Step 10〜11 | ゲームループヘルパー（`myon.time.frame_start()` / `myon.time.frame_wait(frame_start_ms, target_fps)`）。毎フレームの dt 計算と目標 FPS 維持のためのスリープを定型化。`target_fps<=0` は FPS 制限なし。回帰テスト `p51_time_frame_helpers` 追加 | ✅ 実装 |
| Step 12 | SDL_mixer オーディオ（FFI 経由）。`libSDL2_mixer` を `myon.ffi` から叩き、`SDL_Init(AUDIO)`→`Mix_OpenAudio`→`Mix_LoadWAV`→`Mix_PlayChannel`→後片付け、の流れを示すデモ `examples/ffi_sdl_audio.myon`（＋テスト用 `examples/beep.wav`）を追加。オーディオデバイスの無い環境では `Mix_OpenAudio` の失敗を検知してクラッシュせず終了。C 実装の追加はなく examples のみ | ✅ 実装 |

多重代入（`x, y = pair()`）は従来、関数内で使うと外側スコープに同名変数が
存在しても更新せず、関数ローカルに新しいシャドウ変数を作ってしまうバグが
ありました（関数を抜けると外側変数が更新されない不整合）。単純代入が正しく
実装していた spec 9.2 の解決規則（現在スコープに束縛があれば更新／外側に
あればブロック内はシャドウ禁止・非ブロックは外側を更新／どこにも無ければ
新規定義）を、単純代入・多重代入の両方から呼べる共通ヘルパー
`assign_or_define()`（`src/interpreter.c`）に集約し、両者が再び乖離しない
ようにしました。`myon.expose` は仕様 9.1 で「1階層だけ外側へ公開する」
機能と定めているため、本修正の対象外（コード内コメントで明記）です。

Phase 5.1 では加えて、`myon.net`／`myon.http` の実用性を高める2点を実装
しました。**(1) DNS 名前解決**：`src/net.c` の `fill_addr()` を
`getaddrinfo()` ベースへ書き換え、`myon.net.connect(sock, "example.com", 80)`
のようにホスト名で接続できるようにしました（IPv4 リテラルは従来どおりの
高速パスを維持）。**(2) HTTPS/TLS**：新規 `src/tls.{h,c}` に OpenSSL の
薄いラッパ（`SSL_CTX_new`→`SSL_new`→`SSL_set_fd`→`SSL_connect`→
`SSL_read`/`SSL_write`→`SSL_shutdown`）を実装し、`myon.http.get`/`post` が
`https://` URL に対して TLS ハンドシェイクを行うよう分岐させました。SNI と、
システム既定の証明書ストアに対する証明書検証・ホスト名照合をベストエフォート
で有効にしています（詳細・セキュリティ上の注意は
[`docs/myon_spec.md`](docs/myon_spec.md) の 10.7／10.8 節を参照）。

> **セキュリティ**：TLS の証明書検証は簡略化されており、堅牢化された TLS
> クライアントではありません。中間者攻撃に対して脆弱な可能性があるため、
> 信頼できないネットワーク上での機密情報の送受信には使用しないでください。

Phase 5.1 ではさらにゲーム制作支援として、**(3) SDL_Event の読み取り**
（`myon.ffi.read_i32` の追加による out 方向構造体読み取り、
`examples/ffi_sdl_event_loop.myon`）、**(4) ゲームループヘルパー**
（`myon.time.frame_start` / `frame_wait` による dt 計算と FPS 制御）、
**(5) SDL_mixer オーディオ**（FFI 経由での効果音・BGM 再生、
`examples/ffi_sdl_audio.myon`）を実装しました。オーディオ再生は新しい C
モジュールを増やさず、既存の `myon.ffi` から `libSDL2_mixer` を叩く方式です。
`SDL_Init(AUDIO)`→`Mix_OpenAudio`→`Mix_LoadWAV`→`Mix_PlayChannel`→
`Mix_FreeChunk`/`Mix_CloseAudio` の流れを示し、オーディオデバイスの無い環境
（CI コンテナ等）では `Mix_OpenAudio` の失敗を検知してクラッシュせず終了します。

> **実行時の任意依存**：SDL 系のデモ（`examples/ffi_sdl_*.myon`）を実行する
> には `libSDL2-2.0.so.0` が、オーディオデモ `examples/ffi_sdl_audio.myon`
> にはさらに `libSDL2_mixer-2.0.so.0` が必要です（Debian/Ubuntu では
> `sudo apt-get install libsdl2-2.0-0 libsdl2-mixer-2.0-0`）。これらは FFI で
> 実行時に読み込まれる**任意依存**であり、Myon 本体のビルドには不要です
> （未インストールの環境ではデモがその旨を表示してスキップします）。

### Phase 5.2（クリーンビルド修正／再帰スタックオーバーフロー対策／セキュリティ監査）

| ステップ | 内容 | 状態 |
|---|---|---|
| Step 1 | `src/tls.c` のインクルード漏れ修正（`tls_connect()` が `fd_set`／`FD_ZERO`／`FD_SET`／`select()` を使うのに `<sys/select.h>` を include しておらず、クリーンな環境では未定義型・暗黙宣言でビルドが壊れる不具合。`event_loop.c`／`interpreter.c` と同様に `#include <sys/select.h>` を1行追加。`build/` を消してからの再ビルドを `-Wall -Wextra -Wpedantic -Wimplicit-function-declaration` で警告ゼロ確認） | ✅ 実装 |
| Step 2 | 再帰深度リミット追加（スタックオーバーフロー対策）。ツリーウォーク型評価器は Myon の関数呼び出しをそのまま C の呼び出しスタックに乗せるため、深い再帰（約15,000〜20,000段）が C スタックを溢れさせ、診断を出す間もなく OS に `SIGSEGV`（exit 139）で殺されていた。`src/interpreter.c` の `call_function()` にネスト深度カウンタ `Interp.call_depth` を追加し、上限 `#define MYON_MAX_CALL_DEPTH 4000`（マジックナンバー直書き禁止・1箇所定義）を超えたら Step12 の整数オーバーフロー検出と同じ実行時エラー機構（`diag` による行番号・ソース抜粋付き、`exit 1`）で「call stack too deep」を報告し、クラッシュさせずに終了。カウンタは正常 return 経路で必ずデクリメントし、`runtime_error` の `longjmp` で飛ぶ経路はトップレベルの `setjmp` バリア（`interp_run`／`interpret`）で `call_depth=0` にリセット。コルーチン境界（`async`／HTTP コネクション）は独立した C スタックのため深度を save/reset/restore、FFI コールバックは同一 C スタック上なので save/restore（末尾再帰最適化はスコープ外）。回帰テスト `tests/cases/p52_recursion_limit.{myon,err}` 追加 | ✅ 実装 |
| Step 3 | 一般セキュリティ監査（`src/` 全体）。`-Wimplicit-function-declaration` を明示有効化してクリーンビルド警告ゼロを確認。`valgrind --leak-check=full` を全56＋新規テストで実施し、今回追加した再帰チェック経路が新たな definite/indirect リークを生まないことを確認。バッファ境界・整数オーバーフロー・TLS 検証ロジック・エラーパスのリソースリークを目視監査（詳細は下記） | ✅ 実装 |
| Step 4 | 監査で見つかった小修正：HTTP リクエストボディ長の上限追加（`src/interpreter.c` の `http_read_request()`。攻撃者が制御できる `Content-Length` により無制限の `myon_xrealloc` が走り、`Content-Length: 999999999999` 等でメモリを枯渇させられる DoS を防止。ヘッダ既存の 1 MiB 上限に倣い 16 MiB を上限に設定） | ✅ 実装 |

Phase 5.2 は、実際にクリーンな環境（Ubuntu 24.04 + `libssl-dev`）で
`make` → `make test` → valgrind で検証した際に見つかった **2件の具体的な
不具合**の修正と、同種の問題が他に潜んでいないかの**一般監査**です。

**(1) `tls.c` のインクルード漏れ**：`tls_connect()` は非ブロッキング
ハンドシェイクの完了待ちに `select(2)` を使いますが、`<sys/select.h>` を
include していなかったため、ヘッダの暗黙依存に頼れないクリーンな環境で
`fd_set` が未定義型となりビルドが壊れていました。1行の include 追加で解消し、
`build/` を消してからの再ビルドで警告ゼロを確認しています。

**(2) 再帰スタックオーバーフロー**：ツリーウォーク型評価器は Myon の関数
呼び出しを C の呼び出しスタックへ直接積むため、深い再帰がそのまま C スタック
オーバーフローに直結し、`SIGSEGV`（exit 139）でプロセスが落ちていました。
インタプリタ側に深度チェックが無かったのが原因です。`Interp.call_depth` で
ネスト深度を追跡し、`MYON_MAX_CALL_DEPTH`（既定 4000。既定 8 MiB の C
スタックに対し十分な安全余裕を持つ値）を超えたら Step12 の整数オーバーフロー
チェックと同じ体裁の実行時エラー（行番号・ソース抜粋付き、`exit 1`）で
丁寧に終了します。`runtime_error` は `longjmp` でトップレベルまで巻き戻る
ため深いフレームのデクリメントは走りませんが、その場合はトップレベルの
`setjmp` バリアで `call_depth` を 0 にリセットして辻褄を合わせています。

**一般監査の結果**：

- **インクルード漏れの横展開**：`-Wall -Wextra -Wpedantic
  -Wimplicit-function-declaration` でクリーンビルドし、`tls.c` 以外に暗黙宣言
  警告が出るファイルは無いことを確認（警告ゼロ）。
- **メモリ安全性**：全テストケースを `valgrind --leak-check=full` で実行。
  今回追加した再帰チェック経路（深い再帰でエラー終了する経路を含む）で
  `definitely lost`／`indirectly lost` は 0 バイトであることを確認しました。
- **バッファ境界**：`http.c` のリクエストパースは、メソッドを固定長バッファへ
  コピーする箇所でサイズをクランプし、パス・ボディは動的確保（`dupn`）、
  ヘッダは 1 MiB 上限、ディレクトリトラバーサル（`..`）・制御バイトを拒否
  しており堅牢でした。唯一、`http_read_request()` のボディ確保が
  `Content-Length` を無制限に信頼していたため、上限（16 MiB）を追加しました
  （上記 Step 4）。`ffi_call.c`／`net.c` の受信・引数コピーは境界チェック済みでした。
- **整数オーバーフロー**：`myon.ffi.alloc`／`ffi_mem_alloc` は `size<=0` を弾き
  `malloc` 失敗を検知、`ffi_struct_define` の合計サイズ計算はフィールド数が
  ソース由来で現実的に小さく `long long` を溢れさせないことを確認しました。
- **TLS 証明書検証**：`tls.c` は `SSL_VERIFY_PEER` ＋ `SSL_set1_host()` により
  信頼できない証明書チェーン・ホスト名不一致で `SSL_connect` が失敗します。
  エラーを握りつぶしてハンドシェイク成功扱いにする分岐は無く、検証ロジックに
  論理バグは見当たりませんでした（IP／空ホスト時にホスト名照合を省くのは
  仕様どおりの挙動）。TLS 実装の全面的な堅牢化は引き続きスコープ外です。
- **エラーパスでのリソースリーク**：`ffi.c` の `load`/`close`（失敗時にハンドルを
  残さず、`close` 済みスロットは NULL 化して二重 close/use-after-free を防止）、
  `net.c` のソケット生成失敗パス（`getaddrinfo` の `freeaddrinfo`、登録失敗時の
  `close(fd)`）はいずれも漏れなく後片付けしており、修正不要でした。

#### 既知の課題（対応見送り）

- **エラー時（`longjmp`）のスタック巻き戻しで放棄されるメモリ**：Myon の実行時
  エラーは `runtime_error()` が `longjmp` でトップレベルの `setjmp` バリアまで
  一気に巻き戻す設計です。このため、エラーで終了するプログラム（`*.err` テスト
  や深い再帰のリミット到達など）では、途中のスタックフレームが確保した
  `call_env` や一時 `Value` が解放されずに残ります。valgrind 上は
  `definitely/indirectly lost` として計上されますが、これは **Phase 5.2 以前
  から存在する既存の設計上の性質**（同一の挙動を修正前コードでも確認済み）で
  あり、プロセス終了時に OS が回収するため実害はありません。恒久対策には
  各フレームに登録ベースのクリーンアップ（デストラクタチェーン／アリーナ
  アロケータ）を導入する設計変更が必要で規模が大きいため、Phase 2 の P6/P7 と
  同様に **本フェーズでは着手しない**と判断しました。
- **TLS クライアントの堅牢化**：`tls.c` は README 既述のとおりベストエフォート
  実装です。証明書検証の論理バグは無いことを確認しましたが、失効確認（OCSP/CRL）
  やピンニング等の本格的な強化は設計判断を要するため、引き続きスコープ外
  （信頼できないネットワークでの機密送受信には使用しない、という注意を継続）。

## ビルド

```sh
make
```

`myon` という実行ファイルが生成されます。

> **ビルド依存（Phase 5.1）**：HTTPS/TLS 対応（`src/tls.c`）は OpenSSL を
> ネイティブに利用するため、**OpenSSL 開発パッケージ（`libssl-dev` 相当。
> Debian/Ubuntu では `sudo apt-get install libssl-dev`）が必須の依存**です。
> Makefile は常に `-lssl -lcrypto` をリンクします。DNS 名前解決（`getaddrinfo`）
> は libc に含まれるため追加依存はありません。

## 使い方

```sh
./myon examples/hello.myon          # プログラムを実行
./myon --tokens examples/hello.myon # トークン列を出力（Step 1 の確認用）
./myon --tokens -                   # 標準入力から読み込む
./myon                              # 引数なし: 対話式実行（REPL, P3）
```

### 対話モード（REPL, P3）

```sh
$ ./myon
Myon REPL. Type 'exit' or 'quit' to leave (Ctrl+D also works).
myon> x = 1
myon> myon.print(x)
1
myon> exit
```

入力が未完（`()`/`[]`/`{}` が閉じていない）の場合は継続プロンプト `...> ` を表示し、
定義した変数・関数・構造体はセッション終了まで保持されます。実行時エラーが起きても
REPL は終了せず、次の入力を受け付け続けます。

### ファイル I/O（P4）

`module myon.stdio` を宣言すると、`myon.file.read` / `myon.file.write` /
`myon.file.append` / `myon.file.exists` が使えます。読み書きの失敗は Rust/Go 風の
`error` 値として第2戻り値に返り、`myon.if err != myon.nil` で捕捉できます。

### エラーメッセージ（P5）

構文・実行時エラーは、行番号・列番号・該当行のソース抜粋・`^` マーカー付きで
表示されます。

```
myon: syntax error at line 5, column 17: expected ')' to close call (got an integer literal)
     5 | myon.print(1, 2 3)
       |                 ^
```

### 実行例

```sh
$ ./myon examples/hello.myon
Hello Worlddd! 人間!
```

## プロジェクト構成

```
src/
  token.{h,c}        トークン定義
  lexer.{h,c}        字句解析器（Step 1）
  types.{h,c}        型システム（Step 4）
  value.{h,c}        実行時の値表現
  ast.{h,c}          AST ノード定義（Step 2）
  parser.{h,c}       再帰下降パーサー（Step 3）
  env.{h,c}          変数スコープ環境
  interpreter.{h,c}  ツリーウォーク型インタプリタ（Step 4〜17・P4 ファイルI/O）
  common.{h,c}       共通ユーティリティ（メモリ確保・文字列複製）
  diag.{h,c}         診断ヘルパー（P5: ソース抜粋・列番号・トークン名変換）
  ffi_platform.{h,c} C FFI プラットフォーム抽象化層（dlopen/dlsym, Phase3 Step1）
  ffi.{h,c}          C FFI 型・ハンドル管理レイヤ（Phase3 Step2）
  ffi_call.{h,c}     C FFI 呼び出しディスパッチ（libffi 不使用, Phase3 Step3）
  main.c             エントリポイント（ファイル実行 / REPL, P3）
examples/            サンプルプログラム
tests/               回帰テスト（`make test`）
```

## テスト

```sh
make test
```

`tests/cases/` 以下の `*.myon` を実行し、`*.out`（期待出力）または
`*.err`（エラー終了を期待）と比較します。

## ライセンス

Apache License, Version 2.0. 詳細は [`LICENSE`](LICENSE) を参照してください。
