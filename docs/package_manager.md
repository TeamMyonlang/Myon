# Myon パッケージマネージャー仕様

この文書は Myon 内蔵のパッケージマネージャー（`myon pkg`）の**実装済み仕様**を
まとめたものである。利用者向けの短い導入は [`README.md`](../README.md) の
「パッケージ管理」節を、package を配布する開発者向けの手順は
[`package_development.md`](./package_development.md) を参照すること。

本文中のコマンド・ファイル名・import 例は実装および
`tests/cases/pkgproj*/` の fixture と一致する。

---

## 1. 概要と設計方針

- 配布元は **GitHub public repository のみ**。
- 依存は **full commit SHA に固定**して取得・検証する（mutable な branch／tag を
  実行時に再解決しない）。
- package は必ず **project-local**（`<project-root>/.myon/packages/`）に配置する。
  global cache・PATH 変更・system-wide install は行わない。
- package code は **sandbox しない**。package の install／import は、任意の Myon
  code（file I/O・network・FFI 等を含む）の実行と同じ扱いになる。信頼できる
  repository のみを導入すること。
- パッケージマネージャーは Myon script ではなく、**C のネイティブ CLI subsystem**
  として実装されている（`git`／`unzip` などの外部プロセスに依存しない）。

利用者の基本操作:

```sh
myon pkg install https://github.com/owner/repository
```

```myon
module example.tools as tools
```

---

## 2. GitHub source と URL 解決（実装で検証済み）

内部の canonical source 形式（lockfile／manifest に現れる唯一の形式）:

```text
github:<owner>/<repository>@<40桁の小文字16進 commit SHA>
```

例: `github:acme-labs/myon-json@4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910`

`myon pkg install <URL>` が受け付ける利用者向け URL 形状（GitHub 公式の現行仕様を
確認のうえ実装。確認日 2026-08-19）:

```text
https://github.com/<owner>/<repository>
https://github.com/<owner>/<repository>.git
https://github.com/<owner>/<repository>/tree/<ref>
https://github.com/<owner>/<repository>/commit/<sha>
https://github.com/<owner>/<repository>/releases/tag/<tag>
```

URL 検証で拒否するもの: `https` 以外の scheme、`github.com` 以外の host、埋め込み
credentials（`user:pass@`）、query／fragment、制御文字、想定外に深い path。

ref／tag／default branch は、取得時に **immutable な full commit SHA へ解決**する。

解決は次の順で行う（`src/pkg_fetch.c` の `pkg_fetch_resolve_ref`。確認日
2026-08-20）:

0. ref が既に 40 桁 hex（full SHA）なら immutable なので、**ネットワークなし**で
   そのまま採用する（大文字は小文字へ正規化）。
1. **git smart-HTTP の ref discovery を `github.com` に対して行う**（`git clone` が
   使う transport と同じ）:

   ```text
   GET https://github.com/<owner>/<repo>.git/info/refs?service=git-upload-pack
   -> pkt-line 形式で「<40桁SHA> <refname>」を全 ref 分返す
      先頭 ref の capability に "symref=HEAD:refs/heads/<default>" が付く
   ```

   これは既に fetch 許可済みの `github.com` host であり、`api.github.com` の
   非認証 REST rate limit（60 req/時。2025-05 の GitHub changelog
   "Updated rate limits for unauthenticated requests" でさらに厳格化）とは
   **別枠**で、branch／tag／HEAD の解決を 1 リクエストで完結できる。通常運用では
   この経路のみで解決するため、rate limit に当たらない。

2. 1 で解決できなかった場合の **fallback** として、従来どおり GitHub commit API を
   用いる（実装で検証済みの endpoint）:

   ```text
   GET https://api.github.com/repos/<owner>/<repo>/commits/<ref>
   Accept: application/vnd.github.sha
   -> 応答 body は 40 桁の commit SHA
   ```

ref 名の照合順（最も具体的なものを優先）: 完全一致 refname → annotated tag の
peeled `refs/tags/<ref>^{}` → `refs/heads/<ref>` → `refs/tags/<ref>`。default branch
（ref 省略）は HEAD の symref target、無ければ `HEAD` エントリを採用する。

archive は解決済み SHA から package manager が生成する。利用者・開発者が
manifest に任意 URL を書くことはできない:

```text
https://codeload.github.com/<owner>/<repository>/zip/<commit SHA>
```

HTTP status（404＝repo/ref なしまたは private、403／429＝rate limit／forbidden）は
利用者に区別可能な形で報告する。

---

## 2.5. パッケージリスト（`.myon/packages.list`）と shorthand install

URL を毎回書かずに `myon pkg install <owner>/<repo>` の **shorthand** で導入できる。
これは既存の URL 方式（§2）を一切変更せず、その前段に「配布元レジストリで
`<owner>/<repo>` を探して該当 GitHub repo を特定する」層を足したものである。

### `.myon/packages.list`

project root の `.myon/packages.list` に、レジストリ JSON の配布元 URL を **1 行に
1 つ**書く（`src/package.c` の `pkg_packages_list_parse`）。

```text
# 任意のコメント行（'#' 始まり）と空行は無視
https://example.com/xxxx.json
https://ohmygodwhhhhooooo.com/xxxx.json
```

- 各行は `https://` のみ許可（`http://` や制御文字・空白混入は行番号付きで拒否）。
- コメントのみ／空ファイルは「レジストリ 0 件」として扱う。

### レジストリ JSON の形式

レジストリは **純粋なデータ**であり、install 中にコード実行は一切しない
（`src/package.c` の `pkg_registry_parse`。自己完結の strict JSON scanner）。次の 2 形式を
受け付ける。

1. shorthand の配列:

   ```json
   ["acme/myon-json", "owner/pkg", "another/repo"]
   ```

2. 短い alias → shorthand の object:

   ```json
   { "json": "acme/myon-json", "text": "acme/myon-text" }
   ```

各値は `https://github.com/<owner>/<repo>` の `<owner>/<repo>` 部分（GitHub の
`user/repo`）で、`valid_repo_segment` で検証する。値が非文字列・非 shorthand・
制御文字を含む・JSON が壊れている・4 MiB 超・要素過多、などは拒否する。

### `myon pkg install <owner>/<repo>` の解決フロー

`src/pkg_ops.c` の `pkg_ops_install_shorthand`:

1. 引数が shorthand（`://` を含まず `/` が 1 個、両側非空）かを CLI で判定
   （`pkg_arg_is_shorthand`）。URL はこれまでどおり `pkg_ops_install_url` へ。
2. project root（`myon.toml` を上方向に探索、無ければ CWD）を決め、
   `.myon/packages.list` を読む。無ければ「URL で入れてください」と案内して失敗。
3. 各レジストリ URL を **任意 host 許可の HTTPS GET**（`pkg_fetch_https_get_any`）で
   取得。単一レジストリの到達不可／壊れは **warning にして次へ**（fail-closed に
   しない）。全滅なら network error。
4. 最初に `<owner>/<repo>` を含むレジストリで解決し、`https://github.com/<owner>/<repo>`
   を組み立てて、**既存の URL install（§2, §7）へそのまま委譲**する。
   どのレジストリにも無ければ usage error。

`pkg_fetch_https_get_any` は host allow-list を外す点だけが通常の archive fetch と
異なり、HTTPS 限定・`https→http` ダウングレード拒否・credential 埋め込み拒否・
redirect 上限・本文サイズ上限（64 MiB）は同じく適用する。

---

## 3. project manifest: `myon.toml`

```toml
format = 1

[project]
name = "sample-app"
version = "0.1.0"

[dependencies]
acme.json = "github:acme-labs/myon-json@4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910"
```

制約:

- `format = 1` は必須。
- `[project]` の `name` と `version` は必須。
- `[dependencies]` は省略可能。
- dependency key は package name として検証する。
- package name は ASCII の小文字・数字・`.`・`-` に限定。path separator、空白、
  制御文字、`..`、先頭 `.`、末尾 `.` を拒否。
- dependency source は full commit SHA 固定の GitHub source のみ。

`myon pkg install <URL>` は、project manifest が無ければ最小の manifest を自動
生成する。既存 manifest がある場合は、未知構文・コメント・未管理部分を保持したまま
依存だけを追加する（汎用 TOML 全体ではなく strict subset の parser）。重複 key・
unknown key／section・型不一致は行番号付きで拒否する。

---

## 4. package manifest: `package.myon`

各 package archive の package root 直下に置く。

```toml
format = 1

[package]
name = "acme.json"
version = "1.2.0"
module = "acme.json"

[dependencies]
acme.text = "github:acme-labs/myon-text@0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22"
```

制約:

- `format = 1` は必須。
- `name` は install directory の identity。
- `module` は import namespace（`.` 区切りの dotted path、各要素は小文字・数字・
  `-` を許容し安全性検証）。
- `version` は表示・診断用（初版の解決根拠にはしない）。
- dependency source は root manifest と同じ形式。
- install／lock 中に manifest の内容を Myon code として実行しない。install hook／
  build hook／post-install hook は存在しない。

`name`（install identity）と `module`（import identity）は異なってよい。

---

## 5. package の archive layout

GitHub archive の生成 root 名は信用しない。archive 内に単一の top-level directory が
あることを検証し、その直下に以下があることを要求する。

```text
<generated-root>/
  package.myon
  modules/
    <module path>.myon
    <module path>/<submodule>.myon
```

展開後は generated root を除去して配置する。

```text
.myon/packages/acme.json/
  package.myon
  modules/
    acme/
      json.myon
      json/
        parser.myon
```

`module = "acme.json"` の場合の対応:

| import                              | 実 file                                                    |
| ----------------------------------- | ---------------------------------------------------------- |
| `module acme.json as json`          | `.myon/packages/acme.json/modules/acme/json.myon`          |
| `module acme.json.parser as parser` | `.myon/packages/acme.json/modules/acme/json/parser.myon`   |

`.myc` が同梱されていてもよいが、初版の import 実行対象は `.myon` のみ。

---

## 6. lockfile: `myon.lock`

package manager が生成する。手編集は前提にしない。

```toml
format = 1

[[package]]
name = "acme.json"
version = "1.2.0"
module = "acme.json"
source = "github:acme-labs/myon-json"
revision = "4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910"
archive = "https://codeload.github.com/acme-labs/myon-json/zip/4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910"
sha256 = "<64桁の小文字16進 SHA-256>"
dependencies = "acme.text"
```

必須条件:

- deterministic に出力（package entry は name 順、dependency list も sorted）。
- package name は一意。
- root manifest の直接依存と lockfile の top-level package が一致。
- 同じ package name が異なる revision を要求したら conflict。
- 依存 cycle は cycle path を表示して拒否。
- `archive` は `https://codeload.github.com/` 始まりであることを検証。
- install 時、記録した `sha256` と実 archive の hash が一致しなければ install しない
  （SHA-256 は OpenSSL EVP、比較は小文字 16 進 64 桁で統一）。

---

## 7. CLI（`src/main.c` → `src/package.c` / `src/pkg_ops.c`）

```sh
myon pkg install https://github.com/owner/repository   # 主導入（URL 指定）
myon pkg install owner/repository                       # shorthand（packages.list 経由, §2.5）
myon pkg lock                                           # 依存解決 + myon.lock 生成
myon pkg install                                        # lockfile から再現インストール
myon pkg verify                                         # 整合性検査
myon pkg tree                                           # 依存グラフ表示（ネットワーク不要）
```

- `myon pkg install <URL>`: project root 検出 → 無ければ最小 manifest 生成 →
  URL 検証・ref 解決 → archive 取得 → package manifest から name／module 取得 →
  manifest へ依存追加（既存保持）→ full SHA と archive SHA-256 を lockfile へ保存 →
  依存を再帰解決 → 検証・展開 → `.myon/packages/` へ atomic install。
- `myon pkg install <owner>/<repo>`: `.myon/packages.list` の各レジストリを検索して
  `<owner>/<repo>` に対応する GitHub repo を特定し、`https://github.com/<owner>/<repo>`
  を上記 URL install へ委譲する（§2.5）。
- `myon pkg lock`: archive を取得するが install directory は更新しない。
- `myon pkg install`（URL なし）: 既存 lockfile だけを信頼。lockfile が無ければ失敗。
  mutable ref の再解決はしない。manifest を手編集して lock が古い場合は
  `myon pkg lock` を要求する。
- exit code: `0` 成功 / `64` usage / `65` manifest・data / `66` input missing /
  `69` network / `70` integrity。network／manifest／integrity／usage を区別。

---

## 8. ネットワーク層（`src/pkg_fetch.c`）

`myon.http` とは独立した package manager 専用の binary HTTPS GET。応答は C の
binary buffer + length で保持する（NUL 終端 string に丸めない）。

- HTTPS のみ成功。TLS 証明書・hostname 検証は既存 TLS 実装に委譲し、TLS error は
  fail-closed。
- redirect は最大 5 回（`PKG_FETCH_MAX_REDIRECTS`）。
- `https`→`http` ダウングレードを拒否。
- archive／ref 解決／REST の redirect 先 host は allow-list（`github.com` /
  `codeload.github.com` / `api.github.com` / `objects.githubusercontent.com` /
  `raw.githubusercontent.com`）のみ。`Location` の CR／LF／NUL／制御文字を拒否。
- ただし **package-list レジストリの取得**（`pkg_fetch_https_get_any`）だけは
  任意の第三者 host を許可する（レジストリ URL は本質的に外部サイトを指すため）。
  それ以外の安全性（HTTPS 限定・ダウングレード拒否・credential 拒否・redirect 上限・
  本文サイズ上限）は共通。取得内容は JSON メタデータであり、コードとして実行しない。
- status 200 以外を失敗にする。
- `Content-Length` 上限検査と、総ダウンロード量の上限（`PKG_FETCH_MAX_BODY` =
  64 MiB）。
- chunked transfer-encoding をデコード（malformed chunk は拒否）。
- integer overflow／truncated response／malformed header／timeout を検出。

---

## 9. ZIP 安全性（`src/pkg_zip.c`）

vendored ではなく自己完結の security-first ZIP reader。`unzip`／shell／`git` は
起動しない。展開先は必ず staging directory。以下を拒否する。

ZIP Slip（`../`）、absolute path、Windows drive path、backslash 抜け道、NUL／
制御文字、normalized path の重複、symlink／hard link／device／FIFO、encrypted
entry、（未対応なら）ZIP64、複数 top-level root、`package.myon` 欠落、`modules/`
または expected module 欠落、file count 上限超過、compressed／uncompressed size
上限超過、展開比による decompression bomb、壊れた central directory、不正 CRC／
truncated data。

---

## 10. install transaction（`src/pkg_fs.c`）

1. archive 取得 → 2. hash 確認 → 3. `.myon/.staging/<CSPRNG 由来の一意名>/` 作成 →
4. staging へ展開 → 5. manifest／module 検証 → 6. final directory を保全 →
7. staging を final へ atomic／transactional に promote → 8. backup・staging 掃除 →
9. 途中失敗時は既存の正常 install を残す。

固定名の一時 directory・予測可能な一時ファイル名・project root 外の temporary path は
使わない。POSIX と Windows の rename／directory replacement 差は `pkg_fs` に隔離。

---

## 11. module import 解決規則（interpreter、仕様 §6）

module path は prefix dispatch で分類される（`src/interpreter.c`）。

- `myon.*` → builtin module（記録のみ）
- `external.*` → 従来どおり実行 script directory 基準の legacy external module
- それ以外 → **installed package module resolver**

installed package module resolver:

- project root は**実行 script のディレクトリ**から上方向に `myon.toml` を探して
  決める（プロセス CWD に依存しない）。
- `myon.lock` を読み、各 package の宣言 module namespace の**最長一致**で所属
  package を決める。
- 実 file は
  `<project-root>/.myon/packages/<package-name>/modules/<path→スラッシュ>.myon`。
  各 path 要素を安全性検証し、`.myon/packages/` の外へは解決しない。
- package import は **alias 必須**。
- `myon.lock` に無い package は拒否。循環 import は検出してエラー。
- package ごとに独立 namespace（既存 alias module の仕組みを再利用）。global
  namespace を汚染しない。URL 由来の package name と module name を混同しない。

---

## 12. MVM 境界（仕様 §6.3）

初版の package module import 実行対象は tree-walking interpreter（`.myon`）のみ。
`--compile`／`--run-mvm`／`.myc` 実行では、installed package module の取り込みを
**明示的な未対応エラー**にする（`src/mvm_compiler.c`）。曖昧なフォールバックや
`.myc` の自動代替実行は行わない。詳細は [`../known-issues.md`](../known-issues.md)。

---

## 13. セキュリティ境界（まとめ）

- package code は sandbox されない。import＝任意 code 実行。
- 取得は full SHA 固定 + archive SHA-256 検証で改竄・すり替えを防ぐ。
- 展開は path-safety を全面に適用し、project root 外へ書き込まない。
- 失敗時に既存の正常 install を破壊しない。
- lock 中・install 中に package code を実行しない。
