# Myon

半分ネタ、半分実用(？) の、**型に厳密な**プログラミング言語です。

Myon は C で実装されたプログラミング言語です。ソースを評価するツリーウォーク型
インタプリタと、バイトコードを実行する MVM（Myon Virtual Machine）を備えています。

## はじめに

C11 対応コンパイラ、Make、OpenSSL の開発用ヘッダとライブラリを用意し、
リポジトリのルートでビルドします。

```sh
make
./myon examples/hello.myon
```

出力は `Hello Worlddd! 人間!` です。

```myon
system myon.useversion=1
module myon.stdio

name = str("人間")
myon.print("Hello, ", name)
```

## ビルド環境

| 環境 | 依存関係の導入例 |
|---|---|
| Debian / Ubuntu | `sudo apt-get install build-essential libssl-dev` |
| macOS / Homebrew | `brew install openssl@3` |
| Windows / MSYS2 MINGW64 | `pacman -S make mingw-w64-x86_64-gcc mingw-w64-x86_64-openssl` |

macOS では Makefile が Homebrew の `openssl@3` の配置を調べます。
Windows の出力ファイル名は `myon.exe` です。共有リンクした OpenSSL の DLL は、
実行ファイルの配置先または DLL 検索パスから参照できる必要があります。

Linux からの Windows 向けクロスビルドには、MinGW-w64 と Windows 向け OpenSSL が必要です。

```sh
make win-cross WIN_OPENSSL_LDLIBS="-L/path/to/windows/openssl/lib -lcrypt32"
```

追加の include パスは `CFLAGS` で指定できます。CI の設定例は
[heavy-checks.yml](.github/workflows/heavy-checks.yml) にあります。

## 実行とコンパイル

```sh
./myon program.myon
./myon --compile program.myon -o program.myc
./myon program.myc
./myon --dump-bytecode program.myc
./myon --tokens program.myon
./myon --help
./myon --version
```

`.myon` はツリーウォークで実行します。`.myc` という拡張子を持つファイル、または
先頭が `MYC1` のファイルは MVM で実行します。`-` を指定すると標準入力をソースとして読みます。
`--run-mvm program.myon` は、ソースをメモリ上でコンパイルして MVM で実行する検証用の経路です。

`.myc` の実行時には元ソースとの不一致を調べ、検出すると警告します。
`--strict-stale` を付けると不一致をエラーにします。元ソースがない場合はこの検査を省略します。
バイトコードの形式は [MVM 仕様](docs/mvm_spec.md) を参照してください。

スクリプトパスの後ろの引数は `myon.argv()` で取得します。

```sh
./myon program.myon --output result.txt
./myon program.myon -- --help
```

引数なしで起動すると REPL に入ります。変数・関数・構造体はセッション中保持され、
`exit`、`quit`、EOF で終了します。REPL はツリーウォーク実行のみです。

## 言語機能と標準ライブラリ

- 整数・浮動小数点数・文字列・文字・真偽値。異なる型の算術演算には明示的な変換が必要です。
- 条件分岐、ループ、関数、複数戻り値、ラムダ、構造体、継承、ジェネリクス。
- ブロックスコープと `myon.expose`、文字列の式補間。
- 配列・Map、数学、文字列操作、ファイル入出力、時刻、乱数。
- 単一スレッドの協調的な async/await、IPv4 TCP/UDP、HTTP サーバー、HTTP/HTTPS クライアント。
- 共有ライブラリを呼び出す C FFI、メモリブロック、構造体レイアウト、コールバック。

詳細な構文と関数一覧は [言語リファレンス](docs/myon_spec.md)、実装箇所とテストの対応は
[機能・実装状況](docs/features.md) にまとめています。

## 実行経路の制約

MVM は標準ライブラリの一部をツリーウォーク側の実装と共有しますが、
対応範囲と診断がすべてのプログラムで一致するわけではありません。

| 操作 | ツリーウォーク | MVM |
|---|---|---|
| 外部ファイル・パッケージの module 取り込み | 対応 | 未対応エラー |
| 高階ネイティブメソッド `array.map/filter/reduce` での関数呼び出し | ツリーウォーク関数で対応 | VM 関数では未対応エラー |
| FFI コールバック | ツリーウォーク関数で対応 | VM 関数では未対応エラー |
| REPL | 対応 | 未対応 |

保存済み `.myc` は構造体宣言の AST を保持しないため、構造体を使う単体実行には制約があります。
`--run-mvm` と保存済みバイトコードの検証範囲は [MVM 仕様7節](docs/mvm_spec.md#7-対応範囲と制約) を参照してください。

組み込み module 宣言による機能制限など、仕様と実装が一致していない項目もあります。
使用前に [既知の問題](known-issues.md) の未修正項目を確認してください。
文字列は NUL 終端で、任意のバイナリデータを保持する型ではありません。

## パッケージ管理

公開 GitHub リポジトリから、プロジェクト単位でパッケージを取得します。

```sh
./myon pkg install https://github.com/owner/repository
./myon pkg lock
./myon pkg install
./myon pkg verify
./myon pkg tree
```

`install <URL>` は配布元の ref をフルコミット SHA に解決し、依存関係とアーカイブの
SHA-256 を `myon.lock` に記録します。引数なしの `install` は既存ロックを使います。
取得物は `.myon/packages/` に配置され、グローバルインストールや PATH 変更は行いません。

ソースではパッケージが宣言したモジュール名と別名を使います。

```myon
module example.tools as tools
myon.print(tools.square(6))
```

`install` と `lock` はパッケージのコードを実行しません。取り込んだモジュールは実行され、
ファイル・ネットワーク・FFI へのアクセスを制限するサンドボックスはありません。
チェックサムの一致は取得物の一致を調べるもので、コードの発行者や動作を保証するものではありません。

ファイル形式・取得・展開の規則は [パッケージマネージャー](docs/package_manager.md)、
配布側の手順は [パッケージ開発](docs/package_development.md) を参照してください。

## ネットワークと FFI の利用条件

HTTPS クライアントは OpenSSL で証明書チェーンとホスト名を検証し、TLS 1.2 以降を使用します。
OCSP/CRL による失効確認、証明書ピンニング、クライアント証明書、TLS サーバーは未対応です。
HTTP サーバーは HTTP/1.0 で、接続ごとに1リクエストを処理します。

FFI は呼び出し先の C ヘッダを読みません。利用者が ABI、引数型、レイアウト、メモリ寿命を
確認する必要があります。誤ったシグネチャや生アドレスによるアクセスはプロセスを破壊し得ます。
ライブラリ名と配置も OS ごとに異なります。

## サンプル

| ファイル | 内容 |
|---|---|
| [hello.myon](examples/hello.myon) | 基本的な代入と出力 |
| [control_flow.myon](examples/control_flow.myon) | 条件分岐・ループ |
| [http_static_server.myon](examples/http_static_server.myon) | 静的ファイルの HTTP 配信 |
| [http_router_server.myon](examples/http_router_server.myon) | HTTP ハンドラ |
| [http_https_get.myon](examples/http_https_get.myon) | HTTPS クライアント |
| [net_game_echo.myon](examples/net_game_echo.myon) | UDP 通信 |
| [ffi_math.myon](examples/ffi_math.myon) | C 数学ライブラリの呼び出し |
| [ffi_sdl_window.myon](examples/ffi_sdl_window.myon) | SDL2 によるウィンドウ表示 |

サーバーの例は常駐します。FFI・SDL の例には対応する共有ライブラリが必要です。
[cli_progress.myon](examples/cli_progress.myon) は `\r` を使った進捗表示の例です。

## テスト

```sh
make test
make test-pkg
make test-runtime
make test-mvm
make test-mvm-equality
make test-asan
```

テストには Bash、GNU coreutils、diffutils を使います。macOS では
`brew install coreutils` で `gtimeout` を、MSYS2 では `pacman -S coreutils diffutils` で
`timeout`・`diff`・`cmp` を用意します。

`make test` はパッケージの C テスト、メモリ所有権と各種回帰検査、ソース実行、
コンパイラ、実行経路の等価性検査を実行します。ネットワークのケースはローカルの
ソケットを使用します。Linux の `.so` 名に依存するケースは、メインスイートでは
Linux 以外でスキップされます。MVM の未対応機能は等価性スイートで除外されます。
合格・失敗・スキップ・除外の件数は実行ログを確認してください。

`make test-asan` は ASan/UBSan で全スイートを実行します。全体のリーク検出は
`detect_leaks=0` です。個別のリーク検査では、例えば次のように LSan を有効にします。

```sh
make asan
ASAN_OPTIONS=detect_leaks=1 bash tests/numeric_regressions.sh ./myon_asan
```

## CI とリリース

| ワークフロー | 実行条件・内容 |
|---|---|
| [ci.yml](.github/workflows/ci.yml) | main への push と PR。Linux の gcc/clang でビルドとテスト |
| [heavy-checks.yml](.github/workflows/heavy-checks.yml) | main への push と手動実行。sanitizer、Windows クロスビルド、Windows/macOS 実機テスト、benchmark |
| [release.yml](.github/workflows/release.yml) | 手動実行による Linux・Windows・macOS バイナリの配布 |

配布物は [Releases](https://github.com/TeamMyonlang/Myon/releases) から取得できます。
`-dev.` を含むタグは開発版として扱います。タグ名と実行対象 OS・アーキテクチャを確認してください。

## リポジトリ構成と文書

| 場所 | 内容 |
|---|---|
| `src/` | lexer、parser、型・値、インタプリタ、MVM、FFI、ネットワーク、パッケージ管理 |
| `examples/` | 実行例 |
| `tests/` | 回帰テストとフィクスチャ |
| [myon_spec.md](docs/myon_spec.md) | 構文・型・標準ライブラリのリファレンス |
| [mvm_spec.md](docs/mvm_spec.md) | 命令・スタック・バイトコード形式 |
| [features.md](docs/features.md) | 機能と実装・検証箇所の対応 |
| [package_manager.md](docs/package_manager.md) | パッケージ取得・配置・検証の規則 |
| [package_development.md](docs/package_development.md) | パッケージの作成と公開 |
| [known-issues.md](known-issues.md) | 未対応事項、未修正の問題、修正記録 |

## ライセンス

[Apache License 2.0](LICENSE)。Copyright 2026 TeamMyonlang。
