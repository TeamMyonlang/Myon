# Myon package 開発者ガイド

この文書は、Myon package を作って GitHub で配布したい開発者向けの手順書である。
利用者向けの導入は [`README.md`](../README.md) を、実装仕様の全体像は
[`package_manager.md`](./package_manager.md) を参照すること。

package を公開すると、利用者は URL を一度指定するだけで導入でき、その後は package が
宣言した module 名だけで利用できる。

```sh
myon pkg install https://github.com/owner/my-package
```

```myon
module example.tools as tools
```

---

## 1. 最小のディレクトリ構成

package repository の root に `package.myon` を置き、module 実体を `modules/` 以下に
置く。これだけで配布できる。

```text
my-package/                 <- GitHub repository root
  package.myon
  modules/
    example/
      tools.myon            <- module example.tools の実体
      tools/
        util.myon           <- submodule example.tools.util の実体
```

利用者は repository の内部ディレクトリ名や GitHub archive の生成 root 名を覚える
必要はない。package manager が `package.myon` を読み、module 名とファイル配置の
整合性を検証する。

---

## 2. `package.myon` の完全な記述例

```toml
format = 1

[package]
name = "example-tools"
version = "0.1.0"
module = "example.tools"

# 依存 package があるときだけ書く（無ければ丸ごと省略可）
[dependencies]
acme.text = "github:acme-labs/myon-text@0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22"
```

| key       | 必須 | 意味                                                             |
| --------- | ---- | ---------------------------------------------------------------- |
| `format`  | ○    | 常に `1`。                                                       |
| `name`    | ○    | **install identity**。`.myon/packages/<name>/` の directory 名。 |
| `version` | ○    | 表示・診断用。初版では解決根拠にしない。                         |
| `module`  | ○    | **import identity**。利用者が import する dotted module 名。      |

---

## 3. `name` と `module` の違い

この 2 つは役割が異なり、**別の値でよい**。

- `name`（例: `example-tools`）
  install directory の識別子。`myon.toml` の dependency key、`myon.lock` の
  package 一意キー、`.myon/packages/<name>/` の directory 名として使われる。
  ASCII の小文字・数字・`.`・`-` のみ。
- `module`（例: `example.tools`）
  利用者が import 文に書く namespace。`.` 区切りの dotted path。

利用者側の import は必ず `module` を使う（`name` や GitHub URL、install path は
import 文に書かない）。

---

## 4. `modules/` 内の配置と `module ... as ...` の対応

`module = "example.tools"` を宣言した package の場合:

| 利用者の import                          | 置く file の場所                          |
| ---------------------------------------- | ----------------------------------------- |
| `module example.tools as tools`          | `modules/example/tools.myon`              |
| `module example.tools.util as util`      | `modules/example/tools/util.myon`         |
| `module example.tools.json.decode as d`  | `modules/example/tools/json/decode.myon`  |

規則: import path の `.` を `/` に置換し、`modules/` を前置し、末尾に `.myon` を
付けた場所に file を置く。package manager は宣言 module namespace の**最長一致**で
所属 package を決めるので、submodule も同じ package 内に自然に収まる。

module ファイルの中身は普通の Myon source である。

```myon
# modules/example/tools.myon
module myon.stdio

myon.func greet(name: str) ret str { ret "hello, {name}" }
myon.func square(n: int) ret int { ret n * n }
```

利用者側:

```myon
system myon.useversion=1
module myon.stdio
module example.tools as tools

myon.print(tools.greet(str("myon")))   # -> hello, myon
myon.print(tools.square(6))            # -> 36
```

（動作する完全な最小プロジェクトは `tests/cases/pkgproj/` にある。）

---

## 5. 公開から利用者の install まで

1. GitHub に **public repository** を作る。
2. root に `package.myon`、`modules/<...>.myon` を置いて push する。
3. 利用者に repository URL を伝える。

利用者は次を実行するだけでよい。

```sh
myon pkg install https://github.com/owner/my-package
```

package manager が内部で次を行う。

- URL を検証し、ref／default branch を **immutable な full commit SHA** に解決する。
- `https://codeload.github.com/owner/my-package/zip/<sha>` から archive を取得する。
- archive SHA-256 を計算し、`myon.lock` に full SHA と hash を保存する。
- ZIP を安全に検証・展開し、`.myon/packages/example-tools/` に配置する。

利用者は commit SHA や archive URL を手入力しない。

---

## 6. 依存 package を持つ場合

`package.myon` の `[dependencies]` に、同じ GitHub source 形式で宣言する。

```toml
[dependencies]
acme.text = "github:acme-labs/myon-text@0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22"
```

- dependency key は依存 package の `name` と一致させる。
- source は **full commit SHA 固定**の GitHub source のみ（branch／tag／短縮 SHA／
  任意 URL は不可）。
- package manager が依存を再帰的に解決して `myon.lock` に固定する。
- 同じ package が異なる revision を要求すると conflict、循環依存は cycle path 付きで
  エラーになる。

---

## 7. module 名の命名と衝突回避

- `module` は builtin（`myon.stdio` / `myon.math` / `myon.string` / `myon.ffi` /
  `myon.time` / `myon.random` / `myon.net` / `myon.http`）や legacy `external.*` と
  衝突しないよう選ぶ。`myon.` / `external.` で始まる module 名は避けること。
- 短すぎる一般語（`json` 単体など）は他 package と衝突しやすい。`example.tools`
  のように owner／プロジェクトを含む dotted namespace にすると安全。
- import name が既存 namespace と衝突する場合、install／import 時に検出して拒否
  または明瞭な衝突エラーになる。

---

## 8. `.myon` と `.myc` の初版対応範囲

- 初版の import 実行対象は `.myon`。
- `.myc` を同梱してもよいが、package の `.myc` import／link は未対応。
- ツリーウォーク実行（`.myon`）でのみ package module を利用できる。
  `--compile`／`--run-mvm`／`.myc` 実行では明示的な未対応エラーになる（仕様 §6.3）。

---

## 9. セキュリティ上の注意（必読）

- **package code は sandbox されない。** 利用者が package を install／import する
  ことは、あなたの Myon code（file I/O・network・FFI 等を含む）をその利用者の環境で
  実行することと同じ意味になる。
- install／lock は package code を実行しない（hook は無い）が、import 時には実行
  される。利用者に不利益を与える副作用を module top-level に置かないこと。

---

## 10. 開発者がローカルで検証するコマンド

`myon pkg` 系は project 側の操作コマンドで、package repository 単体を直接
検査するものではない。動作確認は「利用者プロジェクトを 1 つ作って install してみる」
のが確実である。ネットワークに接続できる例と、接続不要な例を区別する。

### ネットワーク不要（オフラインで確認できる）

- Myon の実装 repository では、package import の end-to-end fixture がそのまま
  オフラインで動く。

  ```sh
  make                                   # interpreter をビルド
  ./myon tests/cases/pkgproj/main.myon   # 期待出力: hello, myon / 36 / 21
  make test                              # §11 の parser/zip/ops + §6 import を実行
  ```

- 自作 package の `package.myon` の構文と module 配置は、利用者プロジェクトの
  `.myon/packages/<name>/` に手で置いて `./myon` で import してみると、
  ネットワーク無しで確認できる。

### ネットワークが必要（実際の GitHub 取得を確認する）

```sh
mkdir demo && cd demo
myon pkg install https://github.com/<you>/my-package
myon pkg tree            # 解決された依存グラフ（この表示自体はネットワーク不要）
myon pkg verify          # 展開物と myon.toml / myon.lock の整合性
```

`myon pkg install <URL>` は GitHub への接続が必要。`myon pkg tree`／`verify` は
既存の `myon.lock`・install 済みファイルだけを見るのでネットワーク不要。

---

## 11. `.gitignore` の推奨

利用者プロジェクト側では `.myon/packages/`（および `.myon/.staging/`）を Git 管理
から外し、`myon.toml` と `myon.lock` を commit する運用を推奨する。

```gitignore
# Myon: 取得物は追跡しない（lockfile で再現する）
/.myon/packages/
/.myon/.staging/
```
