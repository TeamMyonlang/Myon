# 既知の問題

未修正の不具合と、利用に影響する未対応機能を記載します。修正済みの項目は掲載しません。

## 未修正の不具合

<a id="issue-39"></a>

### 39. 乱数 API の広い int 範囲でのオーバーフロー

状態: 未修正。対象は `myon.random.int` と `myon.random.secure_int` です。

両 API は区間幅を求める際に、符号付き整数で `hi - lo` を計算します。
この差が int の最大値を超えると、C の符号付き整数オーバーフローになります。
`lo <= hi` でもこの問題は発生します。

```myon
module myon.random
lo = -9223372036854775807 - 1
value, err = myon.random.int(lo, 9223372036854775807)
```

この入力で UBSan の `signed integer overflow` を確認しています。
ツリーウォークと MVM は同じ乱数実装を呼び、どちらでも再現します。
`int` を `secure_int` に変更した入力でも同じオーバーフローを確認しています。
修正されるまで、数学上の差 `hi - lo` が 9223372036854775807 を超える区間は使用しないでください。

実装箇所: [src/interpreter.c](src/interpreter.c) の `call_random()`。

## 未対応機能

### 保存済み `.myc` の構造体生成

MYC1 1.4 は構造体宣言の AST を保存しません。ソース内で宣言した構造体を
保存済み `.myc` から生成すると、`struct definitions are not stored in .myc` を含む
実行時エラーで終了します。`.myc` と同じ場所に元ソースを置いても自動では読み込みません。

構造体を使うソースはツリーウォーク、`--run-mvm`、または MVM REPL で実行できます。
外部ファイル・パッケージから読み込む構造体は、そのソースを共通ローダーで読み込む経路です。
ファイル形式と実行経路の詳細は [MVM 仕様7節](docs/mvm_spec.md#7-対応範囲と制約) を参照してください。

IPv6、TLS サーバー、HTTP Keep-Alive の未対応範囲は
[言語リファレンス15節](docs/myon_spec.md#15-現在の制約と検討対象) に記載しています。
