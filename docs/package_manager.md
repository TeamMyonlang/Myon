# パッケージマネージャーの規則とファイル形式

`myon pkg` は公開 GitHub リポジトリからパッケージを取得し、プロジェクト内へ配置します。
ここでは CLI、マニフェスト、ロック、取得・展開、module 解決の規則を説明します。
利用者向けの導入は [README](../README.md#パッケージ管理)、配布側の手順は
[パッケージ開発](package_development.md) を参照してください。

## 1. 処理の範囲

依存はフルコミット SHA に固定し、取得アーカイブの SHA-256 をロックに記録します。
配置先は `<project-root>/.myon/packages/` です。グローバルキャッシュや PATH 変更は行いません。
パッケージマネージャーは C で実装され、取得・ZIP 展開で git/unzip/shell を起動しません。

`install` と `lock` はパッケージのコードを実行しません。module 取り込みはコードを実行します。
そのコードに対する権限分離はありません。13節に実行境界をまとめています。

## 2. GitHub source と URL

マニフェスト内の依存ソースは次の形式です。

```text
github:<owner>/<repository>@<40桁の小文字16進コミットSHA>
```

`install <URL>` は次の形の URL を受理します。

```text
https://github.com/<owner>/<repository>
https://github.com/<owner>/<repository>.git
https://github.com/<owner>/<repository>/tree/<ref>
https://github.com/<owner>/<repository>/commit/<sha>
https://github.com/<owner>/<repository>/releases/tag/<tag>
```

HTTPS 以外、github.com 以外のホスト、埋め込み認証情報、query/fragment、制御文字、
未対応のパス形状は拒否します。ブランチ・タグ・既定 ref は、取得時に GitHub の
コミット API でフル SHA に解決します。

```text
GET https://api.github.com/repos/<owner>/<repo>/commits/<ref>
Accept: application/vnd.github.sha
```

取得アーカイブの URL は解決した SHA から構築します。

```text
https://codeload.github.com/<owner>/<repository>/zip/<commit SHA>
```

利用者が任意のアーカイブ URL を依存 source に指定する形式はありません。
404、403 などの HTTP ステータスは取得失敗として報告します。
404 だけではリポジトリ非公開と ref 不在を区別できません。

## 3. プロジェクトの `myon.toml`

以下のリポジトリと SHA は形式を示す例です。

```toml
format = 1

[project]
name = "sample-app"
version = "0.1.0"

[dependencies]
acme.json = "github:acme-labs/myon-json@1111111111111111111111111111111111111111"
```

`format = 1`、project の name/version は必須です。dependencies は省略できます。
依存キーはパッケージ名として検証します。ASCII 小文字・数字・`.`・`-` を受理し、
先頭・末尾の `.`/`-`、連続する `..`、パス区切り、空白・制御文字は拒否します。

パーサーは TOML 全体ではなく、対応したキー・セクションと値のサブセットを扱います。
重複キー、未知のキー/セクション、型不一致はエラーです。
`install <URL>` はマニフェストがなければ生成し、既存ファイルではコメントなどを
保持しながら管理対象の依存を追加します。未知の構文を受理するという意味ではありません。

## 4. 配布側の `package.myon`

アーカイブのルートに置きます。言語ソースとしては実行しません。

```toml
format = 1

[package]
name = "acme.json"
version = "1.2.0"
module = "acme.json"

[dependencies]
acme.text = "github:acme-labs/myon-text@1111111111111111111111111111111111111111"
```

format/name/version/module は必須です。name は配置と依存解決の識別子、module は
import の名前空間で、異なる値にできます。version は表示・診断用で、依存の選択には
source のコミット SHA を使います。依存の形式はプロジェクトマニフェストと同じです。

module の各要素は小文字で始まり、小文字・数字・`-` を受理する検証規則です。
ただし import 文の構文はハイフンを含む識別子を扱えないため、配布時には小文字・数字の
要素を `.` でつないだ名前を使ってください。

## 5. アーカイブと配置

ZIP の最上位が単一ディレクトリであることを検査し、その下の `package.myon` と
宣言された module のソースを確認します。最上位ディレクトリ名は配置時に取り除きます。

```text
<archive-root>/
  package.myon
  modules/
    acme/
      json.myon
      json/
        parser.myon
```

`name = "acme.json"`、`module = "acme.json"` の対応は次のとおりです。

| 取り込み | 配置済みファイル |
|---|---|
| `module acme.json as json` | `.myon/packages/acme.json/modules/acme/json.myon` |
| `module acme.json.parser as parser` | `.myon/packages/acme.json/modules/acme/json/parser.myon` |

`.myc` を同梱しても、そのファイルを package module としてリンクする機能はありません。

## 6. ロックファイル `myon.lock`

依存解決の結果として生成します。下の revision とハッシュは形式例です。

```toml
format = 1

[[package]]
name = "acme.json"
version = "1.2.0"
module = "acme.json"
source = "github:acme-labs/myon-json"
revision = "1111111111111111111111111111111111111111"
archive = "https://codeload.github.com/acme-labs/myon-json/zip/1111111111111111111111111111111111111111"
sha256 = "0000000000000000000000000000000000000000000000000000000000000000"
dependencies = "acme.text"
```

パッケージと依存一覧は名前順に出力します。パッケージ名は一意で、同じ名前に異なる
revision が要求された場合と循環依存を拒否します。直接依存とロックの整合性も検査します。

アーカイブの SHA-256 は OpenSSL EVP で計算し、小文字16進64桁で比較します。
ロックに記録したハッシュと取得内容が異なる場合は配置を拒否します。
ハッシュが初めてロックに記録される取得時点の信頼性まで、これで保証するものではありません。

## 7. CLI

| コマンド | 処理 |
|---|---|
| `myon pkg install <URL>` | URL を解決し、依存記録・ロック生成・取得・配置を行う |
| `myon pkg lock` | 依存を解決してロックを生成。配置先は更新しない |
| `myon pkg install` | 既存ロックに従って取得・配置。ロックがなければ失敗 |
| `myon pkg verify` | マニフェスト・ロック・配置物の整合性を検査 |
| `myon pkg tree` | ロック内の依存グラフを表示 |

引数なしの install は可変 ref を再解決しません。マニフェストを変更した後は lock を更新します。
verify/tree は既存のロック・配置物を使い、ネットワークを必要としません。

| 終了コード | 分類 |
|---|---|
| 0 | 成功 |
| 64 | コマンドの使い方 |
| 65 | マニフェスト・データ形式 |
| 66 | 入力ファイル不在 |
| 69 | ネットワーク |
| 70 | 整合性検査 |

## 8. ネットワーク

`pkg_fetch.c` は body をバッファと長さで保持する HTTPS GET を実装しています。
`myon.http` の文字列 API とは別経路です。

- TLS の証明書・ホスト名検証は `tls.c` を使用します。
- リダイレクトは最大5回。許可ホストは github.com / codeload.github.com / api.github.com です。
- HTTPS から HTTP への変更と、Location 内の制御文字を拒否します。
- 成功ステータスは200です。応答全体の上限は64 MiBです。
- Content-Length、chunked 応答、不完全な応答、サイズ計算、タイムアウトを検査します。

これらの制限値は `pkg_fetch.c` の定義と併せて確認できます。

## 9. ZIP の検査

`pkg_zip.c` は ZIP/DEFLATE を読み、ステージング内に展開します。
外部の unzip・git・shell は起動しません。検査対象には次を含みます。

- `../`、絶対パス、Windows ドライブパス、バックスラッシュ、制御バイト、正規化後の重複。
- symlink などの非通常ファイル、暗号化、未対応の ZIP64、複数の最上位ディレクトリ。
- マニフェスト・module ソースの欠落、エントリ数・圧縮/展開サイズ・圧縮率の上限超過。
- central directory、CRC-32、不完全なデータ。

受理する形式と上限は実装の条件です。ZIP の検査によって、中にある Myon コードの動作が
安全になるわけではありません。

## 10. 配置の切り替え

`pkg_fs.c` はプロジェクト内の `.myon/.staging/` に一時領域を作ります。
取得・ハッシュ・マニフェスト・module を検証してから、既存配置を退避し、
ステージングから配置先へ切り替えます。失敗時には退避した配置の復元を試みます。

一時名には OS の乱数源を使用します。POSIX と Windows の rename・ディレクトリ置換の
差はこの層で処理します。パッケージの配置切り替えと、依存全体を同時に更新する単一の
トランザクションは区別してください。

## 11. module の解決

組み込み名はインタプリタに登録された myon.* の名前、external.* はスクリプト相対の
外部ファイルとして扱います。それ以外は配置済みパッケージの名前空間から解決します。

1. 実行スクリプトのディレクトリから上へ myon.toml を探索し、プロジェクトを決めます。
2. ロック内の宣言 module と取り込み名を比較し、最長一致するパッケージを選びます。
3. `modules/` に module の `.` を `/` に置換した相対パスと `.myon` を付けて探索します。
4. パス要素、ロック内の登録、alias、循環取り込みを検査します。

パッケージの取り込みには別名が必要です。実行プロセスの CWD ではなく、スクリプトの位置が
探索起点です。パッケージ名・module 名・GitHub のリポジトリ名は区別します。

## 12. MVM での取り込み

パッケージ取り込みはツリーウォーク、`--run-mvm`、保存済み `.myc` に対応します。
MVM も共通のローダーでパッケージのソースを初期化し、関数・値をブリッジ経由で利用します。
探索起点は実行ファイル（ソースまたは `.myc`）のディレクトリで、ロックと配置済みソースが必要です。
[MVM 仕様7節](mvm_spec.md#7-対応範囲と制約) と [既知の問題](../known-issues.md) を参照してください。

## 13. 検証と実行の境界

コミット SHA は取得する revision、SHA-256 はアーカイブの一致を指定します。
これらは署名検証やコードレビューの代替ではありません。

取得・展開の検査と、取り込んだコードの権限は別です。
install/lock に実行 hook はありませんが、import では通常の Myon コードが実行されます。
そのコードは利用者プロセスと同じ権限で動作します。
