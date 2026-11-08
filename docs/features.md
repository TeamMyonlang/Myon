# 機能と実装・検証箇所

この文書は、Myon の機能を実装ファイルと回帰テストに対応付けるための一覧です。
構文・関数の引数と戻り値は [言語リファレンス](myon_spec.md)、MVM の形式は
[MVM リファレンス](mvm_spec.md) を参照してください。
実装が存在することと、すべての入力・環境で検証されていることは区別します。
未対応事項と既知の不具合は [既知の問題](../known-issues.md) に記載しています。

## 言語と値

| 機能 | 主な実装 | 検証例・制約 |
|---|---|---|
| 字句解析・式・文の解析 | `lexer.c`、`parser.c`、`ast.c` | `p1_multiline_args`、`p_generic_comparison_lookahead` |
| int / float / bool / str / char / error | `types.c`、`value.c` | int は符号付き64ビット、float は C の double。文字列は NUL 終端 |
| 型チェックと明示キャスト | `interpreter.c` | `p_numeric_conversion_boundaries`、`numeric_regressions.sh` |
| 整数演算と比較 | `interpreter.c` | `step12_overflow`、`p_int_comparison_precision`。比較は double を経由しない |
| if / elif / else、while、for、break / continue | `parser.c`、`interpreter.c` | `step5_logic`、`examples/control_flow.myon` |
| ブロックスコープ、`myon.expose` | `env.c`、`interpreter.c` | `step8_shadow`、`step8_expose`。エラー発生段階の差は `.mvm-exit` で明示 |
| 関数・複数戻り値・ラムダ | `interpreter.c`、`mvm_compiler.c`、`mvm_vm.c` | 関数型・捕捉・ネイティブ境界の制約は MVM 仕様7節 |
| 構造体・継承・メソッド | `types.c`、`interpreter.c` | `step9_struct`。C FFI の構造体レイアウトとは別機能 |
| ジェネリクス | `parser.c`、`interpreter.c`、`mvm_compiler.c` | `step15_generics`。型制約構文はない |
| 文字列補間 | `interpreter.c`、`mvm_compiler.c` | `{expr}` を評価する。エスケープのバイト検査は `string_regressions.sh` |

`value_copy` / `value_free` は参照カウントを操作します。到達不能な循環は、
Obj と MVM の UpvalueCell を対象とするコレクタで回収します。
この仕組みは、FFI の生アドレスや C 側で独立して確保された資源の管理を代行しません。
出力時には循環と深さを検査し、`<cycle>` / `<...>` で再帰を打ち切ります。
`runtime_memory_tests.c` と `p_tostr_cycle_guard` に対応する検査があります。

## 標準ライブラリ

| 分野 | API の例 | 動作・検証例 |
|---|---|---|
| 標準入出力 | `print`、`println`、`write`、`eprint`、`flush`、`is_tty`、`argv` | `p_stdio_writers`、`p_argv_empty`。write は末尾改行を追加しない |
| ファイル | `myon.file.read/write/append/exists` | `p4_file_io`。read/write/append は結果と error を返す |
| 数学 | `myon.math` | `p35_math_full`、`p35_math_int_precision`、`p4_math_integer` |
| 文字列 | `myon.string` | `p35_string_full`、`p35_string_utf8_length`、`p_float_roundtrip` |
| 配列 | `push/pop/length/sort/sort_desc/reverse/contains/index_of/slice/map/filter/reduce` | `p4_array_methods`、`p4_array_higher_order` |
| Map | `set/get/has/delete/keys/values/length` | `p4_map_methods`、`p_map_index_order`。ハッシュ索引と挿入順のリスト、lengthはO(1) |
| 時刻 | `myon.time.now/now_ms/sleep_ms`、ゲームループ用関数 | `p4_time_basic`、`p51_time_frame_helpers` |
| 乱数 | `myon.random.seed/int/float/secure_int` | `p4_random_basic`。int/float は rand 系、secure_int は OS の乱数源を使う |

`length` は文字列のバイト数、`length_chars` は Unicode コードポイント数です。
`substring` と `index_of` の位置もコードポイント単位で、書記素クラスタ単位ではありません。
`upper/lower` は `unicode.c` と同梱のUnicode 17.0.0表でfull mappingを行います。
Σの文脈と複数文字への展開を扱い、言語別tailoring・正規化は行いません。
[Unicode大小文字変換](unicode.md) にデータと検証範囲を記載しています。

数値関数の戻り値型は関数ごとに異なります。例えば floor/ceil/round/trunc/sign は int、
sqrt/pow は float を返します。最小 int の絶対値、表現不能な gcd、範囲外の整数変換は
実行時エラーです。lcm/pow_int のオーバーフローは第2戻り値の error で報告します。

## 非同期処理とネットワーク

async 関数の呼び出しは Task 値を返します。実行は単一スレッドの協調的イベントループで、
タスクは await・タイマー・I/O 待ちで中断します。CPU 処理のプリエンプションや
マルチコア並列実行は行いません。

| 機能 | 実装 | 検証例・制約 |
|---|---|---|
| POSIX コルーチン | `event_loop.c` の ucontext バックエンド | `p5_async_order`、`p5_async_wait_task` |
| Windows コルーチン | 同ファイルの Win32 Fiber バックエンド | Windows 実機の Heavy checks |
| タスクの再帰上限 | `interpreter.c`、`event_loop.h` | `p_async_recursion_limit`、`p_async_depth_isolation` |
| 完了タスク・未完了コンテキストの回収 | `event_loop.c`、`value.c` | `runtime_memory_tests.c`。スタックと結果ハンドルの寿命を分離。終了時に userdata を解放 |
| IPv4 TCP/UDP | `net.c`、`interpreter.c` | `p5_net_tcp_echo`、`p5_net_udp_echo` |
| ホスト名解決 | `net.c` | `p51_net_dns_localhost`。getaddrinfo を使い、最初の候補を使用 |
| HTTP 静的配信・ハンドラ | `http.c`、`interpreter.c` | `p5_http_serve_static`、`examples/http_router_server.myon` |
| HTTP/HTTPS クライアント | `interpreter.c`、`tls.c` | `p_http_url_hardening`、`p_http_long_header_hardening` |

HTTP は接続ごとに1リクエストを扱います。TLS サーバー、HTTP の Keep-Alive、IPv6 は未対応です。
HTTPS クライアントは TLS 1.2 以降を使い、証明書チェーンと接続先名を検証します。
OCSP/CRL、ピンニング、クライアント証明書は実装していません。
検証処理の詳細は言語リファレンス10.8節と `tls.c` にあります。

## C FFI

FFI のローダーは POSIX の dlopen 系と Windows の LoadLibrary 系を使います。
呼び出しはシグネチャ別の C ラッパーで行い、libffi には依存しません。

| 機能 | 主な実装 | 検証例 |
|---|---|---|
| ライブラリ・関数呼び出し | `ffi_platform.c`、`ffi.c`、`ffi_call.c` | `p_ffi_basic`、`p_ffi_close` |
| 管理メモリブロック・バイト列・整数読み出し | `ffi.c` | `p31_ffi_alloc_free`、`p31_ffi_bytes_crc32`、`p31_ffi_read_i64` |
| C 文字列の読み出し | `ffi.c` | `p31_ffi_read_cstr` |
| 型付きの読み書き | `ffi.c` | `p41_ffi_write_typed` |
| 構造体レイアウト | `ffi.c` | `p41_ffi_struct_dsl` |
| 配列の一括読み書き | `ffi.c` | `p41_ffi_array_bulk` |
| C からのコールバック | `ffi_callback.c`、`interpreter.c` | `p41_ffi_callback`。最大4引数・整数/ポインタ系・int64戻り・16スロット |

構造体レイアウトは i32/i64/f32/f64 のリストから計算します。C ヘッダ・ABI を自動検査する機能は
ありません。管理ブロックの境界検査と、呼び出し先の C コードが行うアクセスは別です。
生アドレスの不正利用や ABI の不一致ではクラッシュし得ます。
NUL を含む値には型付き読み書きを使い、str を任意のバイナリ保存領域として扱わないでください。

SDL2、SDL_mixer、SQLite の利用例は `examples/ffi_*` にあります。
共有ライブラリの配置や型のレイアウトは実行環境で確認する必要があります。

## MVM と実行経路

| 機能 | 実装 |
|---|---|
| AST からのコンパイル・スコープ解決 | `mvm_compiler.c` |
| 命令と定数・チャンク・MYC1形式 | `mvm_bytecode.h`、`mvm_chunk.c` |
| オペランドスタック・呼び出しフレーム | `mvm_vm.c` |
| ネイティブ関数との接続 | `interpreter.h` の `myon_bridge_*` と `interpreter.c` |
| 上位変数の捕捉 | `UpvalueCell`、`MAKE_CLOSURE`、`LOAD_UPVALUE`、`STORE_UPVALUE` |
| ジェネリクス | 型消去によるコンパイル |
| async | タスクごとに VM のオペランド・フレームを保持し、イベントループを共有 |

配列の高階メソッドと FFI コールバックは VM 関数値にも対応します。呼び出しごとに
独立した VM スタックを使い、グローバル変数・捕捉セル・標準ライブラリの状態を共有します。
外部ファイル・パッケージの module 取り込みと MVM 版 REPL は未対応です。`.myc` 単体の構造体実行には、宣言 AST が
保存されないことによる制約もあります。MVM 仕様7節を参照してください。

組み込みmoduleはトップレベルの宣言が必要です。標準ライブラリ呼び出しと明示的な配列/Map生成を
両エンジンで検査します。`module_regressions.sh` は保存済みMYC1 1.2、REPL、外部取り込み、
引数の副作用より先の検査も確認します。宣言はセッション内で共有し、権限分離は行いません。

## パッケージ管理

| 処理 | 実装 | 検査内容 |
|---|---|---|
| マニフェストとロック | `package.c` | 受理する TOML サブセット、名前、ソース形式 |
| SHA-256 | `pkg_hash.c` | 取得アーカイブとロックのハッシュ一致 |
| 取得 | `pkg_fetch.c` | HTTPS、許可ホスト、リダイレクト、応答量、chunked応答 |
| ファイル配置 | `pkg_fs.c` | ステージング、バックアップ、配置切り替え |
| ZIP | `pkg_zip.c` | パス・エントリ形式・サイズ・件数・圧縮率・CRC |
| 依存解決 | `pkg_ops.c` | フル SHA、依存競合・循環、install/lock/verify/tree |
| module 取り込み | `interpreter.c` | スクリプト起点のプロジェクト探索、名前空間の最長一致、alias |

検証は `pkg_unit_tests.c`、`pkg_zip_tests.c`、`pkg_ops_tests.c` と
`tests/cases/pkgproj*/` にあります。install/lock にコード実行フックはありません。
import されたコードのファイル・ネットワーク・FFI アクセスは制限されません。

## プラットフォームと検証範囲

OS 判定と機能マクロは `src/platform.h` にあります。Linux・macOS・Windows には
ビルドとテストの CI ジョブがあります。BSD 向けの実装分岐はありますが、同等の
実機 CI ジョブはありません。CI の成功は、そのジョブで実行されたテストに関する結果です。

`make test` はC単体テスト、所有権・診断・数値・ハーネス検査、ソース実行、
コンパイラ、等価性スイートを実行します。Linuxの `make test-asan` / `make test-lsan` は
同じ完全スイートをASan/UBSan/LSan付きで検査し、PRのgcc CIとHeavy checksで実行します。
構文エラー・コンパイルエラー・実行時エラーも対象です。検出は終了コード99で失敗させ、
意図的なリークによる自己検査と、Value残存数の検査で検出経路と所有権の両方を確認します。
範囲は [README](../README.md#テスト) を参照してください。

メインスイートの `.out` は出力と終了コード0、`.err` は終了コード1/65を検査します。
等価性スイートは両エンジンを1回ずつ実行し、未対応診断を除外として記録します。
エラーケースは終了コード1/65の段階一致を要求します。既知の段階差がある3件は
`.mvm-exit` で両エンジンの期待コードを指定します。診断全文の一致は要求しません。

## 変更記録の参照

過去の文書で使われた Step / Phase は実装当時の区分です。現在の対応範囲は上の一覧、
個々の変更は Git の履歴、修正内容と再現条件は [既知の問題](../known-issues.md) で確認できます。
