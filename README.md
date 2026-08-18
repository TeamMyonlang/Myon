# Myon

半分ネタ、半分実用(？) の、**型に厳密な**プログラミング言語です。

Myon は C 実装のツリーウォーク型インタプリタと、バイトコード VM（MVM）の 2 つの
実行経路を持ちます。`.myon` ソースをそのまま実行することも、`.myc` バイトコードへ
コンパイルして実行することもできます。文字列補間・構造体・ジェネリクス・高階関数・
協調的な async/await・TCP/UDP ソケット・簡易 HTTP サーバー／クライアント・C FFI
（外部共有ライブラリ呼び出し）まで、一通りの機能を備えています。

> ⚠️ 学習・趣味目的の実験的な言語です。本番用途での利用は想定していません。特に
> ネットワーク／TLS 周りはベストエフォート実装で、堅牢化されていません（後述）。

## 特徴

- **厳密な型** — 暗黙の型変換を避け、キャストは明示的に。整数演算はオーバーフローを
  実行時に検出します。
- **わかりやすいエラー** — 構文・実行時エラーは行番号・列番号・ソース抜粋・`^`
  マーカー付きで表示されます。
- **豊富な標準ライブラリ** — `myon.math` / `myon.string` / `myon.array` /
  `myon.map` / `myon.time` / `myon.random` / `myon.file` など。文字列は UTF-8
  文字数ベースで扱えます。
- **async/await** — シングルスレッドの協調的イベントループによる非同期処理
  （OS スレッドは使いません）。
- **ネットワーク** — 低水準ソケット `myon.net`（TCP/UDP、DNS 名前解決対応）と
  簡易 HTTP モジュール `myon.http`（静的配信・ルーティング・HTTP/HTTPS クライアント）。
- **C FFI** — `dlopen`/`LoadLibrary` 経由で外部の共有ライブラリ（`.so`/`.dll`）の
  C 関数を呼び出せます。構造体レイアウト DSL やコールバックにも対応。
- **2 つの実行経路** — ツリーウォーク実行と、`.myc` バイトコードの MVM VM 実行。

各機能の詳細な実装状況・開発の歩みは [`docs/features.md`](docs/features.md) を、
言語仕様の正式な定義は [`docs/myon_spec.md`](docs/myon_spec.md)（言語仕様）と
[`docs/mvm_spec.md`](docs/mvm_spec.md)（MVM バイトコード仕様）を参照してください。

## クイックスタート

```sh
# 1. ビルド
make

# 2. Hello, World を実行
./myon examples/hello.myon
# => Hello Worlddd! 人間!
```

`examples/hello.myon`:

```
system myon.useversion=1
module myon.stdio

x = str("人間")
myon.print("Hello Worlddd! ", x + "!")
```

## ビルド

```sh
make
```

`myon` という実行ファイルが生成されます。

**ビルドに必要なもの**：C コンパイラ（GCC など）と OpenSSL 開発パッケージ。
HTTPS/TLS 対応（`src/tls.c`）が OpenSSL をネイティブに利用するため、`libssl-dev`
相当が**必須依存**です。Makefile は常に `-lssl -lcrypto` をリンクします。

```sh
# Debian / Ubuntu
sudo apt-get install build-essential libssl-dev
```

DNS 名前解決（`getaddrinfo`）は libc に含まれるため追加依存はありません。

### Windows でのビルド

Windows は MinGW-w64（MSYS2 の native ビルド、または Linux からのクロスコンパイル）で
ビルドできます。現状 **Wine 上でのリンク確認まで**しか行っておらず、実機での動作は
未検証です（詳細は [`docs/features.md`](docs/features.md) の Phase 6 を参照）。

```sh
# MSYS2 / MinGW-w64 (native): OpenSSL を導入してから make
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl
make                                         # myon.exe を生成

# Linux からのクロスコンパイル
make win-cross                               # 便利ターゲット
# OpenSSL の検索パスを補う場合:
make win-cross WIN_OPENSSL_LDLIBS="-L/path/to/openssl/lib"
```

Windows ビルドでは Makefile が自動的に出力名を `myon.exe` とし、Winsock2 を
`-lws2_32` でリンクします。OpenSSL を共有リンクした場合は、実行時に
`libssl-*.dll` / `libcrypto-*.dll`（MSYS2 ではランタイム DLL も）を `myon.exe` と
同じディレクトリに置くか、静的リンクしてください。

## 使い方

```sh
# --- 実行 ---
./myon examples/hello.myon          # .myon をツリーウォークで実行
./myon examples/hello.myc           # .myc（MVM バイトコード）を VM で実行

# --- MVM バイトコードの生成・確認 ---
./myon --compile examples/hello.myon              # hello.myc を書き出す（実行はしない）
./myon --compile examples/hello.myon -o out.myc   # 出力先を指定
./myon --dump-bytecode examples/hello.myon        # 逆アセンブルを表示して終了

# --- 対話モード（REPL）---
./myon                              # 引数なしで起動

# --- その他 ---
./myon --tokens examples/hello.myon # トークン列を出力
./myon --tokens -                   # 標準入力から読み込む
./myon --help                       # 使い方を表示
```

ファイル引数は内容と拡張子で振り分けられます。先頭が `MYC1` マジックのバイトコード
（あるいは `.myc` という名前）なら MVM バイトコード VM で、それ以外はツリーウォーク型
インタプリタで実行されます。`-` は常に標準入力（ソース）を意味します。

`.myon` を `--compile` すると MVM バイトコード（`.myc`, マジック `MYC1`）が生成され、
`./myon foo.myc` で実行できます。`.myon`（ツリーウォーク）実行の挙動と一致します。
`.myon` より古い `.myc` を実行しようとすると stale 警告が出ます（`--strict-stale` で
エラー化）。async/await・`myon.net`／`myon.http`・FFI・ジェネリクスは MVM 非対応で、
これらを使うプログラムはツリーウォーク実行（`.myon`）で動かしてください。

### 対話モード（REPL）

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
REPL は終了しません。

### エラーメッセージ

構文・実行時エラーは、行番号・列番号・該当行のソース抜粋・`^` マーカー付きで
表示されます。

```
myon: syntax error at line 5, column 17: expected ')' to close call (got an integer literal)
     5 | myon.print(1, 2 3)
       |                 ^
```

## サンプル

`examples/` ディレクトリに動作するサンプルがあります。

| ファイル | 内容 |
|---|---|
| `hello.myon` | 変数と `myon.print` の基本 |
| `control_flow.myon` | if/elif/else・while・for・break/continue |
| `http_static_server.myon` | 静的ファイルサーバー（`python -m http.server` 相当） |
| `http_router_server.myon` | パスに応じて分岐するルーティング＋404 |
| `http_https_get.myon` | HTTPS クライアント（`myon.http.get`） |
| `net_game_echo.myon` | UDP による座標 echo デモ |
| `ffi_math.myon` / `ffi_zlib_version.myon` | C FFI（`libm` / `libz`） |
| `ffi_sdl_window.myon` ほか `ffi_sdl_*.myon` | SDL2 を FFI で叩く GUI/オーディオデモ |
| `snake5.myon` | 総合サンプル |

サーバー系・SDL 系・ネットワーク系のサンプルは、常駐したり外部ライブラリ
（`libSDL2` など）を必要としたりするため、回帰テストには含めていません。SDL 系デモの
実行には `libSDL2-2.0.so.0`（オーディオは加えて `libSDL2_mixer-2.0.so.0`）が必要です。

> **セキュリティ注意**：`myon.http` の TLS（HTTPS）は簡略化されたベストエフォート
> 実装で、堅牢化された TLS クライアントではありません。中間者攻撃に対して脆弱な
> 可能性があるため、信頼できないネットワーク上での機密情報の送受信には使わないで
> ください。

## テスト

```sh
make test
```

`tests/cases/` 以下の `*.myon` を実行し、`*.out`（期待出力）または `*.err`
（エラー終了を期待）と比較します。あわせて `.myon`／`.myc` の等価性検証スイート
（`tests/run_mvm_tests.sh`）も実行され、ツリーウォーク実行と MVM バイトコード実行の
出力一致を確認します。

> FFI（`libm`/`libz` 等を要する）やネットワーク（ソケットを開く）を伴う一部ケースは、
> 実行環境によってはテストハーネスが自動的に除外します。除外は失敗ではなく、対応
> ライブラリ・権限のある環境ではパスします。

### Sanitizer（ASan/UBSan）テスト

メモリ破壊・未定義動作の再発を検出するため、AddressSanitizer と
UndefinedBehaviorSanitizer 付きでテストスイートを走らせるターゲットがあります。

```sh
make test-asan     # myon_asan をビルドし、ASan/UBSan 下で全テストを実行
make asan          # myon_asan バイナリだけをビルド
```

`make test-asan` は `-fsanitize=address,undefined` でビルドした `myon_asan` を
一時的に `./myon` として差し替えてスイートを実行し、終了時に元のバイナリへ戻します
（テストスクリプトは無改修）。Sanitizer が問題を検出するとステップは異常終了します。
`tests/cases/p_overflow_*` と `p_float_roundtrip` は、過去に踏んだオーバーフロー／
未定義動作（境界計算・整数リテラル・数値変換・float 往復）の回帰ケースです。

## CI・リリース

GitHub Actions で以下のワークフローを用意しています（`.github/workflows/`）。

| ワークフロー | トリガー | 内容 |
|---|---|---|
| `ci.yml` | push(main) / PR | gcc・clang の 2 系統で `make` + `make test`（軽量な毎コミットチェック） |
| `heavy-checks.yml` | push(main) / 手動 | sanitizer（`make test-asan`）、Windows クロスビルド、ベンチマーク、macOS ビルド確認 |
| `release.yml` | 手動のみ | Linux `myon` と Windows `myon.exe` を配布リリースとして公開 |

- **heavy-checks** は重いジョブ群です。Windows クロスビルドは MinGW-w64 で
  Windows 版 OpenSSL をソースからクロスビルド（`actions/cache` でキャッシュ）し、
  `-lcrypt32` を含めてリンクします。macOS ジョブは、現状 `src/ffi_platform.c` の
  macOS 分岐が FFI 未対応スタブのため**ビルド確認のみ**です。
- **benchmark** ジョブはリリース公開に依存しない独立ジョブで、計測結果を
  artifact として保存します。

### ダウンロード（リリース）

ビルド済みバイナリは [Releases](../../releases) ページから入手できます。

- **正式版**（タグに `-dev.` を**含まない** 例: `v1.3.0`）は安定版です。
- **開発版**（タグに `-dev.` を含む 例: `v1.3.0-dev.20260818.1`）は
  開発中のスナップショットで、既知バグを含む・不安定な可能性があります。

各リリースには Linux 版 `myon-linux-x86_64` と Windows 版
`myon-windows-x86_64.exe` の両方が添付され、ログイン不要でダウンロードできます。

リリースはメンテナが `release.yml` を手動実行し、`pre`／`stable` とバージョン番号
（`X.Y.Z`）を指定して発行します。`pre` はタグに日付＋連番が付与され、
GitHub 上で pre-release バッジが付きます。

## プロジェクト構成

```
src/
  token.{h,c}        トークン定義
  lexer.{h,c}        字句解析器
  types.{h,c}        型システム
  value.{h,c}        実行時の値表現
  ast.{h,c}          AST ノード定義
  parser.{h,c}       再帰下降パーサー
  env.{h,c}          変数スコープ環境
  interpreter.{h,c}  ツリーウォーク型インタプリタ
  common.{h,c}       共通ユーティリティ（メモリ確保・文字列複製）
  diag.{h,c}         診断ヘルパー（ソース抜粋・列番号・トークン名変換）
  ffi_platform.{h,c} C FFI プラットフォーム抽象化層（dlopen ｜ Windows: LoadLibrary）
  ffi.{h,c}          C FFI 型・ハンドル管理レイヤ
  ffi_call.{h,c}     C FFI 呼び出しディスパッチ（libffi 不使用）
  ffi_callback.{h,c} C FFI コールバック（静的トランポリン）
  event_loop.{h,c}   協調的イベントループ（Linux: ucontext ｜ Windows: Win32 Fiber）
  net.{h,c}          低水準ソケット myon.net（Linux ｜ Windows: Winsock2）
  http.{h,c}         簡易 HTTP モジュール myon.http
  tls.{h,c}          HTTPS/TLS ラッパ（OpenSSL）
  mvm_bytecode.h     MVM オペコード定義
  mvm_chunk.{h,c}    MVM チャンク・定数プール・`.myc` シリアライズ
  mvm_compiler.{h,c} AST→MVM バイトコードコンパイラ
  mvm_vm.{h,c}       MVM バイトコード VM ランタイム
  main.c             エントリポイント（CLI 解析・.myon/.myc 振り分け・REPL）
examples/            サンプルプログラム
docs/                言語仕様・機能一覧
  myon_spec.md       言語仕様
  mvm_spec.md        MVM バイトコード仕様
  features.md        機能一覧・実装状況・開発の歩み
tests/               回帰テスト（`make test`）
  cases/             `.myon`／`.out`／`.err` の回帰ケース
  run_tests.sh       ケース実行ハーネス
  run_mvm_tests.sh   `.myon`／`.myc` 等価性検証スイート
  bench_mvm.sh       ベンチマーク（`BENCH_FIB` / `BENCH_LOOP` で負荷調整）
.github/workflows/   GitHub Actions
  ci.yml             push(main)/PR の gcc・clang ビルド＋テスト
  heavy-checks.yml   sanitizer／Windows クロス／ベンチ／macOS ビルド
  release.yml        手動リリース（Linux/Windows バイナリを Releases に公開）
```

## ドキュメント

- [`docs/myon_spec.md`](docs/myon_spec.md) — 言語仕様（型・構文・標準ライブラリ・
  EBNF 文法など）
- [`docs/mvm_spec.md`](docs/mvm_spec.md) — MVM バイトコード仕様
- [`docs/features.md`](docs/features.md) — 機能一覧・実装状況・開発の歩み

## ライセンス

Apache License, Version 2.0. 詳細は [`LICENSE`](LICENSE) を参照してください。

Copyright 2026 TeamMyonlang
