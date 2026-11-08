# Unicode大小文字変換

`myon.string.upper` / `myon.string.lower` は Unicode 17.0.0 の Default Case Conversion
に従う full mapping を使用します。ツリーウォークとMVMは同じ実装を呼びます。

## 動作

- `upper("café")` は `CAFÉ`、`upper("Straße")` は `STRASSE` です。
- `lower("İ")` は `i` と結合点（U+0307）の2コードポイントです。
- `lower("ΟΣ")` は `ος`、`lower("ΟΣΑ")` は `οσα` です。Σの語末判定では、
  Cased / Case_Ignorable のプロパティを元の入力に対して調べます。
- 補助平面を含むUnicodeスカラー値を扱います。変換のない文字は保持します。
- 不正UTF-8のバイトは1バイトずつ保持し、Σの文脈では区切りとして扱います。
  途中切れ、過長形式、サロゲート、U+10FFFFより大きい値もこの扱いです。

言語別のtailoringは行いません。トルコ語・アゼルバイジャン語・リトアニア語などの
言語固有規則やOSのロケールは使いません。正規化・case folding・titlecaseは別の操作で、
このAPIには含みません。大文字化と小文字化が互いの逆変換になるとは限りません。
`str` は引き続きNUL終端で、NULを含むバイナリ列を表す型には変わりません。

## 実装とデータ

[src/unicode.c](../src/unicode.c) は厳密なUTF-8の復号、変換表の二分探索、
Σの前後関係、UTF-8への再符号化を行います。変換後のバイト数を事前に求め、
サイズ加算のオーバーフローを検査してから領域を確保します。

変換表は [src/unicode_case_data.inc](../src/unicode_case_data.inc) に同梱しています。
ビルド・実行時にPython、Unicodeデータのダウンロード、追加ライブラリは必要ありません。
Unicodeのバージョンを更新する際は、生成スクリプトの入力ハッシュとテストも更新します。

## 表の再生成

公式の [Unicode 17.0.0 UCD](https://www.unicode.org/Public/17.0.0/ucd/) から、
以下の3ファイルを同じディレクトリへ保存します。

- `UnicodeData.txt`: 単純な文字変換。
- `SpecialCasing.txt`: 複数文字への変換と条件付き変換。
- `DerivedCoreProperties.txt`: Cased / Case_Ignorable。

リポジトリのルートで実行します。ここでだけPython 3を使用します。

```sh
python3 tools/generate_unicode_case.py /path/to/ucd
python3 tools/generate_unicode_case.py /path/to/ucd --check
```

[生成スクリプト](../tools/generate_unicode_case.py) は入力3ファイルのSHA-256を固定し、
バージョンや内容の異なる入力を拒否します。Σの文脈依存規則はC側で実装し、
言語別規則は生成対象から除外しています。生成結果の差分はコミットしてレビューします。

## 検査

`make test-unicode` は複数文字への展開、Σの文脈、結合文字、補助平面、
Unicode 16/17で追加された対応、ロケール切り替え、不正UTF-8、長い入力を検査します。
`make test` にも含まれ、sanitizer版は `make test-asan` とLinuxの `make test-lsan` で実行します。

[言語の回帰ケース](../tests/cases/p_unicode_case_mapping.myon) では、両エンジンで
同じ出力になることを検査します。これは保存済みバイトコードの実行にも使えるケースです。

公式データと全スカラー値を照合する追加検査は、Linuxでは次のように再現できます。
入力データのハッシュを検査し、1文字の変換を全件、文脈を含む入力を別の参照実装で照合します。
これは通常のビルド・CIの依存関係には含めません。

```sh
cc -std=c11 -O2 -fPIC -shared -Isrc src/unicode.c src/common.c -o /tmp/myon-unicode.so
python3 tests/unicode_conformance.py /path/to/ucd /tmp/myon-unicode.so
```

## ライセンス

表はUnicode公式データを変換・抽出したものです。データの著作権はUnicode, Inc.に帰属し、
[Unicode License V3](../third_party/unicode/LICENSE.txt) に従って配布します。
Myon側の実装コードはリポジトリのApache License 2.0に従います。
