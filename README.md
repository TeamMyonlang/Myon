# Myon

半分ネタ、半分実用(？) の、**型に厳密な**プログラミング言語です。

Myon は C 実装のツリーウォーク型インタプリタと、バイトコード VM（MVM）の 2 つの
実行経路を持ちます。`.myon` ソースをそのまま実行することも、`.myc` バイトコードへ
コンパイルして実行することもできます。文字列補間・構造体・ジェネリクス・高階関数・
協調的な async/await・TCP/UDP ソケット・簡易 HTTP サーバー／クライアント・C FFI
（外部共有ライブラリ呼び出し）まで、一通りの機能を備えています。

## 特徴

- **厳密な型** — 暗黙の型変換を避け、キャストは明示的に。整数演算はオーバーフローを
  実行時に検出します。
- **わかりやすいエラー** — 構文・実行時エラーは行番号・列番号・ソース抜粋・`^`
  マーカー付きで表示されます。
- **豊富な標準ライブラリ** — `myon.math` / `myon.string` / `myon.array` /
  `myon.map` / `myon.time` / `myon.random` / `myon.file` など。文字列は UTF-8
  文字数ベースで扱えます。
- **CLI ツール制作支援** — 改行なし出力 `myon.write`（`\r` 上書き・プログレスバー
  向け）、改行付き `myon.println`、明示フラッシュ `myon.flush`、標準エラー出力
  `myon.eprint`、端末判定 `myon.is_tty`、コマンドライン引数 `myon.argv()`
  （詳細は [`docs/myon_spec.md`](docs/myon_spec.md) §10.1）。
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

GitHub の package を使いたい場合は [パッケージ管理（`myon pkg`）](#パッケージ管理myon-pkg)
を参照してください（詳細な仕様は [`docs/package_manager.md`](docs/package_manager.md)、
package を自作したい場合は [`docs/package_development.md`](docs/package_development.md)）。

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

Windows は 実機で動作済みです(Github Action workflowにて)

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

# --- スクリプトへ引数を渡す（myon.argv() で受け取る）---
./myon script.myon --foo bar        # スクリプトパスの後ろは全部スクリプト用引数
./myon script.myon -- --help -v     # `--` 以降は先頭が `-` でも素通し

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
./myon --version                    # バージョンを表示（-v も可）
```

ファイル引数は内容と拡張子で振り分けられます。先頭が `MYC1` マジックのバイトコード
（あるいは `.myc` という名前）なら MVM バイトコード VM で、それ以外はツリーウォーク型
インタプリタで実行されます。`-` は常に標準入力（ソース）を意味します。

`.myon` を `--compile` すると MVM バイトコード（`.myc`, マジック `MYC1`）が生成され、
`./myon foo.myc` で実行できます。`.myon`（ツリーウォーク）実行の挙動と一致します。
`.myon` より古い `.myc` を実行しようとすると stale 警告が出ます（`--strict-stale` で
エラー化）。async/await・`myon.net`／`myon.http`・FFI・ジェネリクス・クロージャ
（上位変数キャプチャ）・高階関数は **MVM でも対応済み**です（イベントループや標準
ライブラリはツリーウォーク実装をブリッジ経由で共有します）。ただし *VM で作った
関数値をツリーウォークのネイティブに渡してコールバック実行させる*ケース
（`array.map`／`filter`／`reduce` などの高階ネイティブメソッド、`myon.ffi.make_callback`）
だけはエンジン境界をまたげないため MVM では明示的なエラーになります。該当プログラムは
ツリーウォーク実行（`.myon`）で動かしてください。外部モジュール取り込み
（`module external.* as ...`）とインストール済みパッケージの module 取り込み
（`module <package-module> as ...`）も MVM 非対応で、`--compile`／`--run-mvm`
では明示的なエラーになります（仕様 §6.3）。

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

## パッケージ管理（`myon pkg`）

GitHub の public repository を配布元とする、プロジェクト単位のパッケージ管理を
内蔵しています。URL を一度指定するだけで導入でき、導入後は package が宣言した
module 名だけで利用できます。

```sh
# URL を指定して導入（初回・通常利用）
myon pkg install https://github.com/owner/repository

# 明示的な操作
myon pkg lock       # 依存を解決して myon.lock を（再）生成
myon pkg install    # 既存の myon.lock だけを信頼して再現インストール
myon pkg verify     # myon.toml / myon.lock / 展開物の整合性を検査
myon pkg tree       # ロック済み依存グラフを表示（ネットワーク不要）
```

導入した package は、`myon.toml`／`myon.lock` に記録され、実体は
`<project-root>/.myon/packages/<package-name>/` にプロジェクトローカルで
配置されます（グローバルキャッシュや PATH 変更は行いません）。

利用側の source からは、package manifest が宣言した **module 名**を alias 付きで
取り込みます（GitHub URL やインストール先 path は import 文に書きません）。

```myon
module example.tools as tools           # package のルート module
module example.tools.util as util       # サブ module

myon.print(tools.greet(str("myon")))
myon.print(util.triple(7))
```

module import の解決規則（仕様 §6）:

- 取り込み path は、`myon.lock` に記録された各 package の宣言 module namespace の
  **最長一致**で所属 package を決めます。
- package の実 file は
  `<project-root>/.myon/packages/<package-name>/modules/<path→スラッシュ>.myon`
  に解決され、各 path 要素は安全性検証されます（`..`／絶対 path／区切り混入を拒否し、
  `.myon/packages/` の外へは解決しません）。
- project root は**実行スクリプトのディレクトリ**から上方向に `myon.toml` を探して
  決めるため、プロセスの作業ディレクトリに依存しません。
- package import は **alias 必須**、`myon.lock` にない package は拒否、
  循環 import は検出してエラーにします。
- **⚠️ package code は sandbox されません。** package の module を import することは、
  任意の Myon code（file I/O・network・FFI 等を含む）を実行することと同じ扱いです。
  信頼できる repository のみを導入してください。
- `--compile`／`--run-mvm`／`.myc` 実行では package module 取り込みは未対応で、
  明示的なエラーになります（仕様 §6.3）。ツリーウォーク実行（`.myon`）で利用して
  ください。

GitHub からの archive 取得（仕様 §7）は package manager 専用の C 層
（`src/pkg_fetch.c`）で行い、既存の `myon.http` とは独立です。HTTPS のみを許可し、
TLS 証明書・ホスト名を検証（fail-closed）、`https`→`http` ダウングレード拒否、
redirect は最大 5 回かつ GitHub の archive host のみ許可、`Location` の制御文字拒否、
`Content-Length`／総ダウンロード量の上限（64 MiB）、chunked 転送のデコードなどを
実装しています。

`.myon/packages/` は通常 `.gitignore` 対象とし、`myon.toml` と `myon.lock` を
Git 管理する運用を推奨します（本リポジトリの [`.gitignore`](.gitignore) にも
`.myon/packages/` を記載しています）。

より詳しい情報は次のドキュメントを参照してください。

- 仕様・内部実装: [`docs/package_manager.md`](docs/package_manager.md)
  — GitHub URL 解決・`myon.toml`／`myon.lock`／`package.myon` の文法・network 層・
  ZIP 安全性・install トランザクション・module import 解決規則（§6）
- package を作って公開する方法: [`docs/package_development.md`](docs/package_development.md)
  — ディレクトリ構成・`package.myon` の書き方・module 名と path の対応・
  公開／導入フロー・ローカル検証手順

## サンプル

`examples/` ディレクトリに動作するサンプルがあります。

| ファイル | 内容 |
|---|---|
| `hello.myon` | 変数と `myon.print` の基本 |
| `cli_progress.myon` | `myon.write`/`flush`/`is_tty`/`eprint`/`argv` を使った進捗バー CLI |
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

> **セキュリティ**：`myon.http` の TLS（HTTPS）クライアントは**フェイルクローズ**
> で動作します。システムの信頼ストアに対してサーバ証明書チェーンを検証し
> （`SSL_VERIFY_PEER`）、接続前に期待するホスト名（DNS 名は `SSL_set1_host`、
> IP リテラルは証明書の iPAddress SAN）を照合、**TLS 1.2 を最低バージョン**として
> 強制（RFC 8996）、SNI は DNS 名のみ送信（RFC 6066）、TLS 圧縮・再ネゴシエーション
> を無効化し、ハンドシェイクにタイムアウトを設けます。証明書が失効・ホスト名不一致・
> 自己署名などの場合はハンドシェイクを中断し、理由付きのエラーを返します
> （中間者攻撃に対する基本的な保護を提供します）。
>
> ただし OCSP/CRL による失効確認・証明書ピンニング・クライアント証明書には非対応の
> コンパクトな実装です。高保証が必要な用途ではこの制限を考慮してください。また
> `myon.http.get`/`post` に渡す URL は、ホスト・ポート・パスに含まれる制御文字
> （CR/LF 等）や範囲外ポートを拒否し、HTTP ヘッダ／リクエストインジェクションを
> 防ぎます。

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
- [`docs/package_manager.md`](docs/package_manager.md) — GitHub ベース package
  管理の仕様・内部実装（`myon pkg` / `myon.toml` / `myon.lock` / module import §6）
- [`docs/package_development.md`](docs/package_development.md) — package 開発者向け
  ガイド（package の作り方・公開／導入・ローカル検証）

## ライセンス

Apache License, Version 2.0. 詳細は [`LICENSE`](LICENSE) を参照してください。

Copyright 2026 TeamMyonlang
