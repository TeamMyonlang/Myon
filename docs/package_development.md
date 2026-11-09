# パッケージの作成と公開

Myon のパッケージは、`package.myon` と `modules/` を持つ公開 GitHub リポジトリです。
このガイドは配布側のファイル配置、依存宣言、公開、検証を説明します。
取得・配置処理の規則は [パッケージマネージャー](package_manager.md) を参照してください。

## 1. ディレクトリ構成

```text
my-package/
  package.myon
  modules/
    example/
      tools.myon
      tools/
        util.myon
```

ルートモジュールが `example.tools` の場合、そのファイルは
`modules/example/tools.myon` です。サブモジュール `example.tools.util` は
`modules/example/tools/util.myon` に置きます。
GitHub の ZIP が付ける最上位ディレクトリ名はインストール先に引き継ぎません。

## 2. マニフェスト

`package.myon` は拡張子が `.myon` ですが、言語のソースとしては実行されません。
パッケージマネージャーが扱う TOML サブセットで記述します。

```toml
format = 1

[package]
name = "example-tools"
version = "0.1.0"
module = "example.tools"
```

| キー | 用途 |
|---|---|
| `format` | ファイル形式。現在は `1` |
| `name` | パッケージの識別子。依存キーと配置先ディレクトリ名に使う |
| `version` | 表示・診断用のバージョン。取得するコミットの選択には使わない |
| `module` | 取り込み時の名前空間 |

4項目はすべて必須です。重複キーや未対応のセクションはエラーになります。

## 3. パッケージ名とモジュール名

`name` と `module` は別の識別子です。同じ文字列にする必要はありません。

- `name = "example-tools"` は `.myon/packages/example-tools/`、ロック内の識別子、
  利用者の `myon.toml` の依存キーに使います。
- `module = "example.tools"` は `module example.tools as tools` と書くときに使います。

パッケージ名には ASCII 小文字・数字・`.`・`-` が使えます。先頭・末尾の `.` と `-`、
連続する `..`、パス区切り、空白・制御文字は拒否されます。

モジュール名は小文字から始まる要素を `.` で連結します。マニフェストの検証では
要素内の数字・`-` も受理しますが、言語の import 構文は識別子として解析します。
現状の取り込みに使う名前には、小文字で始まる英小文字・数字の要素を使用してください。

## 4. ソースと取り込み

`modules/example/tools.myon` の例です。

```myon
myon.func greet(name: str) ret str {
    ret "hello, {name}"
}

myon.func square(n: int) ret int {
    ret n * n
}
```

インストール後、利用者は宣言されたモジュール名に別名を付けて取り込みます。

```myon
module myon.stdio
module example.tools as tools

myon.print(tools.greet(str("myon")))
myon.print(tools.square(6))
```

取り込み文には GitHub URL、パッケージ名、インストール先のパスを書きません。
名前空間が複数のパッケージに一致する場合は、最長一致で所属を解決します。

## 5. 公開と取得

1. 公開 GitHub リポジトリに `package.myon` と `modules/` を配置します。
2. 変更をコミットして push します。
3. 利用者にリポジトリ URL と利用するモジュール名を伝えます。

以下の `<owner>` とリポジトリ名は配布先に置き換えます。

```sh
myon pkg install https://github.com/<owner>/my-package
```

取得時に ref または既定ブランチをフルコミット SHA に解決します。
アーカイブの SHA-256 と依存関係をロックに記録し、検証後に配置します。
公開 URL が受理されることと、アーカイブがパッケージとして検証を通ることは別です。

## 6. 依存パッケージ

依存がある場合だけ `[dependencies]` を追加します。以下の配布元と SHA は形式の例です。

```toml
[dependencies]
acme.text = "github:acme-labs/myon-text@1111111111111111111111111111111111111111"
```

依存キーは依存先の `name` に合わせます。ソースには40桁の小文字16進コミット SHA を
指定します。ブランチ・タグ・短縮 SHA・任意のアーカイブ URL は指定できません。
依存は再帰的に解決され、同一パッケージに異なる revision が要求された場合や
循環がある場合は解決エラーになります。

## 7. 名前空間

`myon.*` と `external.*` は組み込み・外部ファイル用の名前空間として使われます。
配布モジュールには、例えば `example.tools` のようにプロジェクトを識別できる名前を付けます。
名前空間の重複は依存解決や取り込み時に検査されます。

## 8. 実行経路

パッケージモジュールはツリーウォークと MVM の両方で利用できます。
MVM は配置済みソースを共通ローダーでツリーウォーク実行し、公開された関数・値を利用します。
パッケージに同梱した `.myc` の静的リンクは行いません。

## 9. コード実行と権限

`lock` と `install` はマニフェストを解析し、パッケージのコードを実行しません。
インストール用の hook はありません。

モジュールを取り込むと、そのトップレベルの文が実行されます。
実行環境のファイル・ネットワーク・FFI アクセスを制限するサンドボックスはありません。
取り込み時に発生する副作用と、必要な共有ライブラリ・外部サービスを配布文書に記載してください。

## 10. 動作確認

リポジトリ内のフィクスチャを使う取り込み確認は、ネットワークなしで実行できます。

```sh
make
./myon tests/cases/pkgproj/main.myon
make test-pkg
```

自作パッケージは、別の利用者プロジェクトを作って取得・取り込みを検査します。

```sh
mkdir demo
cd demo
myon pkg install https://github.com/<owner>/my-package
myon pkg tree
myon pkg verify
```

`install <URL>` はネットワークを使います。`tree` と `verify` は、ロックと
配置済みファイルを使って検査します。テスト用にファイルを手で配置しただけでは、
GitHub からの取得や ZIP 展開の経路は検証されません。

## 11. バージョン管理

利用者プロジェクトでは `myon.toml` と `myon.lock` をコミットし、取得物とステージングを
追跡対象から外す運用にできます。

```gitignore
/.myon/packages/
/.myon/.staging/
```

ロックを変更した場合は、その差分にある配布元・revision・ハッシュも確認してください。
