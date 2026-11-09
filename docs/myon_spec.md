# Myon 言語リファレンス

## 0. この文書について

この文書は Myon の構文、型、標準ライブラリ、実行方法を説明します。
実装は C のツリーウォーク型インタプリタと MVM で構成されます。
MVM の命令とファイル形式は [MVM リファレンス](mvm_spec.md)、実装箇所・検証例は
[機能一覧](features.md) を参照してください。

言語の規則と実装の不一致は [既知の問題](../known-issues.md) に記録しています。
実行エンジン間のエラー段階には制約があります。検証範囲は機能一覧と既知の問題を参照してください。

### 0.1 表記

`ret T` は戻り値の型、`ret T, error` は結果とエラーの2値を表します。
API 表の `int|float`・`?`・`func`・`array of T` は説明上の表記を含みます。
それぞれを型注釈としてそのまま書けるとは限りません。
コードブロックには、単体で実行できる例と、宣言・外部ライブラリなどを前提とする断片があります。

## 1. 字句規則

### 1.1 コメント

`#` と `//` は行末まで、`/* ... */` は複数行のコメントです。
コメント内の改行と、文の区切りとしての改行を区別してください。

```myon
# 行コメント
// 行コメント
/* 複数行の
   コメント */
```

### 1.2 識別子

英字または `_` で始まり、英数字と `_` を続けます。大文字と小文字は区別します。
数字で始まる名前は識別子として扱いません。

### 1.3 リテラル

| 型 | 例 | 備考 |
|---|---|---|
| int | `1`, `42`, `0x1F`, `0o17` | 10進数・16進数・8進数（14.1節参照） |
| float | `3.14`, `1.5e10` | 通常表記・指数表記（14.1節参照） |
| str | `str("人間")` | コンストラクタ経由 |
| char | `char("a")` | 現実装の変換は1バイト単位 |
| bool | `true`, `false` | 直接代入可（int同様の扱い） |

浮動小数点数には `1.5e10` や `1e3` の指数表記も使えます。
文字列リテラルは式補間の対象です。`\n`・`\r`・`\t`・`\\`・`\"` をそれぞれ
改行・復帰・タブ・バックスラッシュ・引用符へ変換します。その他の `\q` や `\0` などは
バックスラッシュを含めて保持します。NUL を生成するエスケープはありません。

### 1.4 文の区切り

改行または `;` で文を区切ります。`()` と `[]` の内側の改行は区切りにしません。
`{}` の内側では文の区切りとして改行を使えます。

```myon
module myon.stdio
x = 1; y = 2
myon.print(
    x,
    y
)
```

### 1.5 キーワードと予約名

主な字句キーワードは次のとおりです。`myon.print` などは専用トークンを持ち、
`myon.println` などは修飾名として解決される組み込み名です。
実際のトークン化は `src/lexer.c` の PLAIN_KW / MYON_KW で確認できます。

```text
system module myon ret str char int float bool void error then range self as
true false
myon.if myon.elif myon.else myon.while myon.for myon.in
myon.break myon.continue myon.func myon.struct myon.array myon.map
myon.print myon.input myon.and myon.or myon.not myon.nil
myon.expose myon.lambda myon.extends myon.async myon.await
```

## 2. 型と変換

### 2.1 値の型

| 型 | 内容 |
|---|---|
| int | 符号付き64ビット整数 |
| float | C の double による浮動小数点数 |
| bool | true / false |
| str | NUL 終端のバイト列として保持する文字列 |
| char | 文字用の型。現実装の変換は1バイト単位 |
| error | 失敗を表すメッセージ |
| void | 戻り値なしの宣言に使う型 |

配列・Map・構造体・関数・Task も実行時の値です。
Task は async 関数の呼び出し結果で、通常は利用者が型注釈に書きません。

### 2.2 演算の型

異なる型の算術演算は自動変換しません。例えば int + float は型不一致です。
数値型の変換を許容する標準ライブラリ関数は、その関数の規則に従います。
整数の加減乗除、符号反転などで結果が範囲を超える場合は実行時エラーです。
int 同士の比較は double を経由せず、int64 のまま行います。

### 2.3 明示的な変換

```myon
n = int("123")
s = str(123)
c = char(65)
```

`int(float)` は0方向への切り捨てです。NaN/Infinity と int64 範囲外の値は拒否します。
`int(str)` は10進文字列を解析し、解析失敗・範囲外を実行時エラーにします。
`myon.string.to_int` は別の API で、エラーを第2戻り値として受け取れます。

`char(int)` は下位8ビット、`char(str)` は先頭バイトを使います。空文字列は拒否します。
Unicode コードポイントを char に変換する API ではありません。
`str(value)` は値の表示表現を生成します。任意のバイナリ列を保存する機能ではありません。

### 2.4 myon.nil

nil は値がないことを表すセンチネルとして、error の成功側や Map の未検出結果などに使います。
通常変数への単一の nil 代入は拒否します。変数の宣言には初期値が必要です。
エラーとの比較には `err == myon.nil` / `err != myon.nil` を使います。

## 3. 変数と演算子

```myon
count = 1
message = str("hello")
count += 2
```

型注釈を付ける場合は `count: int = 1` と書きます。
注釈を省いた束縛は評価結果の値から作られます。関数呼び出しや計算結果の代入にも使います。
静的な型推論でプログラム全体の型を事前に確定する機構ではありません。

### 3.1 演算子

算術は `+ - * /`、比較は `== != < > <= >=`、複合代入は `+= -= *= /=` です。
論理演算は中置の `myon.and` / `myon.or` と前置の `myon.not` を使います。
and/or は右辺を評価しない場合があります。not を後置には書けません。

### 3.2 優先順位

| 高い順 | 演算子 |
|---|---|
| 1 | 呼び出し `()`、添字 `[]`、メンバー `.` |
| 2 | 単項 `-` |
| 3 | `* /` |
| 4 | `+ -` |
| 5 | 比較 |
| 6 | `myon.not` |
| 7 | `myon.and` |
| 8 | `myon.or` |

優先順位を明示する場合は括弧を使います。`myon.not a == b` は比較結果に対する否定です。

## 4. 文字列補間

文字列内の `{expr}` を評価して文字列化します。`{{` と `}}` はリテラルの波括弧です。

```myon
module myon.stdio
name = str("myon")
count = 2
myon.print("name={name}, next={count + 1}")
```

現実装の補間は最初の閉じ波括弧を区切りに使います。入れ子の波括弧を含む任意の式が
常に解析できるという規則ではありません。エスケープは文字列のリテラル部分で処理し、
補間した値の中のバックスラッシュを再変換しません。

## 5. 制御構文

### 5.1 条件分岐

if の then は省略可能、elif の then は必須、else には条件を付けません。

```myon
module myon.stdio
a = 2
b = 1
myon.if a > b {
    myon.print("a is greater")
} myon.elif a < b then {
    myon.print("b is greater")
} myon.else {
    myon.print("equal")
}
```

### 5.2 ループ

range の終点は含みません。配列の for-in は要素を順に束縛します。

```myon
module myon.stdio
myon.for i myon.in range(0, 3) {
    myon.print(i)
}

n = 0
myon.while n < 3 {
    n += 1
    myon.if n == 2 then { myon.continue }
    myon.print(n)
}
```

break は最も内側のループを抜け、continue は次の反復へ進みます。

## 6. 関数とエラー

### 6.1 宣言と呼び出し

引数は `名前: 型`、戻り値は `ret 型` と書きます。

```myon
module myon.stdio
myon.func add(a: int, b: int) ret int {
    ret a + b
}
myon.print(add(2, 3))
```

戻り値を持たない宣言には `ret void` を使います。

### 6.2 複数戻り値

```myon
module myon.stdio
myon.func divide(a: int, b: int) ret int, error {
    myon.if b == 0 then { ret 0, error("division by zero") }
    ret a / b, myon.nil
}
result, err = divide(10, 2)
myon.if err != myon.nil then { myon.print(err) }
myon.print(result)
```

error 値の返却は、呼び出し元で検査するための規約です。
一方、型不一致や通常の算術オーバーフローなどの実行時エラーは処理を中止します。
この2つの経路を区別してください。

### 6.2.1 診断

構文エラーは行・列とトークンの説明を表示します。
実行時エラーは行を表示し、ソースを参照できる場合は抜粋とキャレットを追加します。
実行時のキャレットは列情報がないため行頭を指します。

```text
myon: runtime error at line 4: division by zero
     4 | myon.print(a / b)
       | ^
```

REPL はエラー後も次の入力を受け付けます。ファイル実行では終了コードで失敗を報告します。
全診断の本文や発生段階が実行エンジン間で一致するわけではありません。

### 6.3 ラムダ

ラムダは定義元の変数を捕捉します。定義元の呼び出しやブロックが終了した後も、
関数値が生きている間は捕捉した束縛を保持します。兄弟クロージャによる変更は共有されます。
ツリーウォークでは Env を、MVM では捕捉セルを参照として保持し、不要な循環も回収します。

```myon
module myon.stdio
threshold = 10
is_over = myon.lambda(x: int) ret bool { ret x > threshold }
myon.print(is_over(15))
```

ラムダやネスト関数は上位変数を参照します。
MVM の捕捉セルとネイティブへの受け渡し制約は MVM リファレンス7節を参照してください。

## 7. 配列

`myon.array(T)` は要素型情報を持つ空配列を作ります。

```myon
module myon.array
module myon.stdio
xs = myon.array(int)
xs.push(1)
xs.push(2)
myon.print(xs[0])
myon.print(xs.length())
```

添字は0始まりです。負の添字や要素数以上のアクセスはエラーになります。
代入やコピーは配列オブジェクトを共有し、複製を作る操作とは異なります。

### 7.1 メソッド

| メソッド | シグネチャ | 説明 |
|------|-----------|------|
| `push` | `(v: T) ret void` | 末尾に追加（型不一致は実行時エラー） |
| `pop` | `() ret T` | 末尾を削除して返す |
| `length` | `() ret int` | 要素数 |
| `sort` | `() ret void` | 昇順にその場でソート（破壊的）。要素は全て`int`/`float`/`str`である必要がある（混在した`int`/`float`は`double`比較、`str`は`strcmp`のバイト列辞書順）。それ以外の型が混ざると`runtime_error` |
| `sort_desc` | `() ret void` | 降順にその場でソート（破壊的）。比較ロジックは`sort`と同じ |
| `reverse` | `() ret void` | 要素順をその場で反転（破壊的） |
| `contains` | `(v) ret bool` | `v`と等しい要素が存在するか（`value_equal`） |
| `index_of` | `(v) ret int` | `v`と最初に等しい要素の位置（0-indexed）。無ければ`-1` |
| `slice` | `(start: int, len: int) ret myon.array(T), error` | `start`番目から`len`個を取り出した新しい配列。`start`/`len`が負、または`start+len`が配列長超過なら`error` |
| `map` | `(f) ret myon.array(?)` | 各要素に1引数関数/ラムダ`f`を適用した新しい配列。要素型情報は先頭結果から取得（空配列は型情報なし） |
| `filter` | `(f) ret myon.array(T)` | `f`の結果がtruthyな要素だけを残した新しい配列（要素型は元と同じ） |
| `reduce` | `(f, init) ret ?` | `acc=init`から左畳み込み（各要素で`acc = f(acc, elem)`）。空配列は`init`をそのまま返す |

sort/sort_desc/reverse は元の配列を変更します。push/pop も要素数を変更します。
slice/map/filter は新しい配列を返します。map/filter/reduce はツリーウォークと MVM の
両方の関数値を呼び出せます。MVM のコールバックは捕捉セルとグローバル変数を共有します。

```myon
module myon.array
module myon.stdio
xs = myon.array(int)
xs.push(3)
xs.push(1)
xs.sort()
part, err = xs.slice(0, 1)
myon.print(part)
```

## 8. 構造体と継承

フィールドとメソッドを宣言し、フィールド名付きの引数でインスタンスを作ります。

```myon
module myon.stdio
myon.struct Person {
    name: str
    age: int
    myon.func greet() ret void {
        myon.print("name=", self.name)
    }
}
p = Person(name=str("myon"), age=20)
p.greet()
```

`myon.extends Parent` で親構造体を指定します。親と同名のフィールドは宣言できません。
インスタンスの操作には self を使います。MVM で保存した `.myc` だけを実行する場合の
宣言情報の制約は MVM リファレンス7節を参照してください。

## 9. スコープ

### 9.1 束縛の有効範囲

ブロック内で作った束縛は、そのブロックを出ると外から参照できません。
`myon.expose` は束縛を1つ外側へ持ち出します。

```myon
module myon.stdio
{
    exported = 7
    myon.expose exported
}
myon.print(exported)
```

### 9.2 代入と同名束縛

代入先の解決規則は単一代入・複数代入に適用します。

1. 現在のスコープに束縛があれば更新します。
2. 外側にだけ束縛がある場合、明示的なブロックではシャドーイングとして拒否します。
   通常の関数本体などでは外側の束縛を更新します。
3. どこにもなければ現在のスコープに作ります。

構造体メソッドは外側への代入探索の境界です。self のフィールド更新と、
同名のグローバル変数の更新は別の操作になります。
この規則には MVM がコンパイル時、ツリーウォークが実行時に拒否するケースがあります。
等価性テストでは既知の段階差を `.mvm-exit` に明示し、双方の終了コードを検査します。

## 10. 標準ライブラリ

### 10.1 標準入出力

出力関数は各引数を文字列化し、順に連結します。名前付き引数には対応しません。
これらの入出力関数にはトップレベルの `module myon.stdio` 宣言が必要です。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `myon.print`   | `myon.print(...) ret void`   | stdout へ出力し、**末尾に改行を付ける** |
| `myon.println` | `myon.println(...) ret void` | stdout へ出力し、末尾に改行を付ける（`print` の明示的な別名） |
| `myon.write`   | `myon.write(...) ret void`   | stdout へ出力し、**改行を付けない** |
| `myon.eprint`  | `myon.eprint(...) ret void`  | **stderr** へ出力し、末尾に改行を付ける。診断・進捗表示を stdout（パイプ加工対象）と分離する |
| `myon.flush`   | `myon.flush() ret void`      | stdout のバッファを即座に書き出す。`myon.write` で1行を更新し続ける場合に有用 |
| `myon.is_tty`  | `myon.is_tty() ret bool`     | stdout が対話端末（TTY）に接続されていれば `true`。パイプ・リダイレクト時に ANSI 制御文字を抑制する判断に使う |
| `myon.input`   | `myon.input(prompt: str) ret str` | prompt を表示して stdin から1行読み込む |
| `myon.argv`    | `myon.argv() ret myon.array(str)` | スクリプトへ渡されたコマンドライン引数（§10.1.1）を返す。常に配列（空配列もあり得る）を返し、`nil` は返さない |

文字列リテラルの `\r` は復帰文字になります。例えば `myon.write("\rloading")` は
カーソルを行頭へ戻して出力します。実際の表示は端末の制御文字の扱いに従います。

### 10.1.1 スクリプト引数

スクリプトパスの後ろのトークンは、そのスクリプトの引数です。
`--` は引数の開始を明示し、その区切り自体は argv に含めません。

```sh
myon script.myon --output result.txt
myon script.myon -- --help
```

### 10.2 ファイル入出力

`module myon.file` を宣言して使用します。読み書き API は結果と error を返します。
処理対象のパスは実行環境のファイルシステムに解決され、サンドボックスによる制限はありません。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `myon.file.read`   | `myon.file.read(path: str) ret str, error`            | ファイル全体を `str` として読み込む |
| `myon.file.write`  | `myon.file.write(path: str, content: str) ret bool, error`  | 上書き書き込み（成功時 `true`） |
| `myon.file.append` | `myon.file.append(path: str, content: str) ret bool, error` | 末尾に追記（成功時 `true`） |
| `myon.file.exists` | `myon.file.exists(path: str) ret bool`                | 存在確認（エラーは返さない） |

```myon
module myon.file
module myon.stdio
ok, err = myon.file.write("/tmp/myon-example.txt", "hello")
myon.if err == myon.nil then {
    content, read_err = myon.file.read("/tmp/myon-example.txt")
    myon.print(content)
}
```

Windows の `/tmp/` 接頭辞は実装内で TEMP/TMP のディレクトリへ変換します。
一般の相対パスはプロセスの CWD に依存します。パッケージ探索の起点とは異なります。

### 10.3 C FFI

`module myon.ffi` は共有ライブラリへの接続を扱います。
POSIX は dlopen/dlsym/dlclose、Windows は LoadLibraryA/GetProcAddress/FreeLibrary を使います。
ライブラリ名と配置は OS に依存します。libffi には依存しません。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `myon.ffi.load`   | `myon.ffi.load(path: str) ret int, error`             | 共有ライブラリを読み込み、ハンドルID（`int`）を返す |
| `myon.ffi.close`  | `myon.ffi.close(handle: int) ret bool, error`         | ハンドルを閉じる（成功時 `true`） |
| `myon.ffi.call_i` | `myon.ffi.call_i(handle: int, name: str, sig: str, ...) ret int, error`   | `int` を返すC関数を呼ぶ |
| `myon.ffi.call_d` | `myon.ffi.call_d(handle: int, name: str, sig: str, ...) ret float, error` | `double` を返すC関数を呼ぶ |
| `myon.ffi.call_p` | `myon.ffi.call_p(handle: int, name: str, sig: str, ...) ret int, error`   | ポインタを返すC関数を呼ぶ（戻り値は `int` として表現） |
| `myon.ffi.call_v` | `myon.ffi.call_v(handle: int, name: str, sig: str, ...) ret bool, error`  | `void`（戻り値なし）のC関数を呼ぶ（成功時 `true`） |

引数型を1文字ずつ並べた sig を渡します。空 sig は引数なしです。

| 文字 | 対応するC型 | Myon側の型 |
|------|-------------|-----------|
| `i`  | `long long`（整数）  | `int` / `bool` |
| `d`  | `double`             | `float`（`int` も可、自動的に `double` へ変換） |
| `p`  | `void*`（不透明ポインタ） | `int`（ポインタ値をそのまま整数として保持） |
| `s`  | `const char*`        | `str` |
| `b`  | `void*`（Myon が確保したメモリブロックを指す） | `int`（`myon.ffi.alloc` が返すブロックID） |

引数は最大6個で、整数系 i/p/s/b の後ろに d を並べる形式に対応します。
異なる並び順は受理されない場合があります。C の構造体の値渡し・値返しは未対応です。
シグネチャ別ラッパーの一覧は `src/ffi_call.c` にあります。

i は Myon 側の int64 を使います。呼び出し先が32ビット int を要求する場合も、
ABI と符号・値域を利用者が確認してください。「下位32ビットに収まれば常に正しい」
という規則ではありません。

```myon
module myon.ffi
module myon.stdio
h, err = myon.ffi.load(str("libm.so.6"))
myon.if err == myon.nil then {
    result, call_err = myon.ffi.call_d(h, str("sqrt"), str("d"), 16.0)
    myon.print(result)
    myon.ffi.close(h)
}
```

このライブラリ名は Linux の例です。close 後のハンドルは呼び出しに使えません。

#### 10.3.1 管理メモリと生アドレス

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `myon.ffi.alloc`       | `myon.ffi.alloc(size: int) ret int, error`               | `size` バイトのゼロ初期化済みメモリを確保し、非負のブロックID（`int`）を返す。`size` が0以下なら `error` |
| `myon.ffi.free`        | `myon.ffi.free(block_id: int) ret bool, error`           | ブロックを解放する（成功時 `true`）。無効／解放済みIDは `error` |
| `myon.ffi.read_cstr`   | `myon.ffi.read_cstr(addr: int, max_len: int) ret str, error` | 生アドレス `addr` が指すNUL終端C文字列を `str` として読み出す |
| `myon.ffi.write_bytes` | `myon.ffi.write_bytes(block_id: int, offset: int, data: str) ret bool, error` | ブロック内の `offset` から `data` のバイト列を書き込む（成功時 `true`）。範囲外は `error` |
| `myon.ffi.read_bytes`  | `myon.ffi.read_bytes(block_id: int, offset: int, len: int) ret str, error`    | ブロックの `[offset, offset+len)` を `str` として読み出す。範囲外は `error` |
| `myon.ffi.read_i64`    | `myon.ffi.read_i64(block_id: int, offset: int) ret int, error`                | ブロックの `offset` から8バイトをリトルエンディアンの `int` として読み出す。範囲外は `error` |
| `myon.ffi.read_i32`    | `myon.ffi.read_i32(block_id: int, offset: int) ret int, error`                | ブロックの `offset` から4バイトをリトルエンディアンの int32 として読み出し、符号拡張して `int` を返す（符号拡張する）。範囲外は `error` |

alloc が返すブロックID、load のハンドルID、生アドレスは別の値です。
シグネチャ b は管理ブロックのIDを実アドレスへ変換します。
p と call_p の戻り値は生アドレスです。

read_cstr は生アドレスを読みます。addr=0 は拒否しますが、それ以外のアドレスが
有効かどうかを保証する機能はありません。読み出す最大量は1〜16,777,216バイトです。
read_bytes/read_i64/read_i32 は管理ブロックのIDと範囲を検査します。

str は NUL 終端です。write_bytes のデータ長は strlen によって決まり、
read_bytes の結果も NUL 以降を通常の文字列操作で保持できません。
任意バイト列には管理ブロックと型付きの読み書きを使用します。

C の出力ポインタを受け取る例は `examples/ffi_sqlite_open.myon` です。
ポインタの幅・エンディアン・寿命は呼び出す ABI に合わせる必要があります。

#### 10.3.2 型付きメモリ・レイアウト・コールバック

##### 型付き書き込み

| 関数 | シグネチャ | 説明 |
| ---- | ---------- | ---- |
| `myon.ffi.write_i64` | `myon.ffi.write_i64(block_id: int, offset: int, v: int) ret bool, error` | `offset` に `v` を8バイト int64（LE）として書き込む |
| `myon.ffi.write_i32` | `myon.ffi.write_i32(block_id: int, offset: int, v: int) ret bool, error` | `offset` に `v` を4バイト int32（LE、上位32bitは切り捨て）として書き込む |
| `myon.ffi.write_f64` | `myon.ffi.write_f64(block_id: int, offset: int, v: float) ret bool, error` | `offset` に `v` を8バイト IEEE754 double として書き込む |
| `myon.ffi.write_f32` | `myon.ffi.write_f32(block_id: int, offset: int, v: float) ret bool, error` | `offset` に `v` を（double→float 変換後の）4バイト IEEE754 float として書き込む |

i32/i64 はリトルエンディアンで書きます。NUL を含む値でも型の幅を書き込みます。
境界検査は管理ブロック内のアクセスに対する検査で、外部 C コードのアクセスを制限しません。

##### 構造体レイアウト

| 関数 | シグネチャ | 説明 |
| ---- | ---------- | ---- |
| `myon.ffi.struct_def`   | `myon.ffi.struct_def(name: str, field_kinds: array of str) ret int, error` | `field_kinds`（例 `["i32","i32","i32","i32"]`）からレイアウトを登録し、合計サイズ（バイト数）を返す。不正な型文字列は `error` |
| `myon.ffi.struct_alloc` | `myon.ffi.struct_alloc(name: str) ret int, error` | 定義済み構造体のサイズ分を確保し、ブロックIDを返すラッパー。未定義名は `error` |
| `myon.ffi.struct_write` | `myon.ffi.struct_write(block_id: int, name: str, field_index: int, v: int\|float) ret bool, error` | `field_index` 番目のフィールドへ、その型に応じて書き込む。値の型とフィールド型が矛盾すれば `error` |
| `myon.ffi.struct_read`  | `myon.ffi.struct_read(block_id: int, name: str, field_index: int) ret int\|float, error` | 同フィールドを読み出す。戻り値のスカラー型（`int`/`float`）はフィールド型から実行時に決まる |

対応する要素は i32/i64/f32/f64 です。4バイト型は4境界、8バイト型は8境界へ配置し、
末尾を最大アライメントの倍数に揃えます。例えば i32/i64 はオフセット0/8、合計16バイトです。
同名の定義は上書きします。struct_read のスカラー型は登録フィールドの型で決まります。
C ヘッダ、packing 指定、実際の ABI と一致しているかは別途確認してください。

##### 配列の読み書き

| 関数 | シグネチャ |
| ---- | ---------- |
| `myon.ffi.write_array_i32` | `(block_id: int, offset: int, values: array of int) ret bool, error` |
| `myon.ffi.write_array_i64` | `(block_id: int, offset: int, values: array of int) ret bool, error` |
| `myon.ffi.write_array_f32` | `(block_id: int, offset: int, values: array of float) ret bool, error` |
| `myon.ffi.write_array_f64` | `(block_id: int, offset: int, values: array of float) ret bool, error` |
| `myon.ffi.read_array_i32`  | `(block_id: int, offset: int, count: int) ret array of int, error` |
| `myon.ffi.read_array_i64`  | `(block_id: int, offset: int, count: int) ret array of int, error` |
| `myon.ffi.read_array_f32`  | `(block_id: int, offset: int, count: int) ret array of float, error` |
| `myon.ffi.read_array_f64`  | `(block_id: int, offset: int, count: int) ret array of float, error` |

i32/f32 は1要素4バイト、i64/f64 は8バイトです。
要素数とブロックのサイズを検査します。内部では単一値の読み書きを繰り返します。

##### コールバック

| 関数 | シグネチャ | 説明 |
| ---- | ---------- | ---- |
| `myon.ffi.make_callback` | `myon.ffi.make_callback(fn: func, arg_count: int) ret int, error` | Myon の関数値 `fn` をコールバックとして登録し、C から見た生の関数ポインタを `int`（アドレス値）で返す。この値はそのまま `'p'` 引数として `call_i`/`call_v` 等に渡せる。スロット枯渇や `arg_count` 範囲外（0〜4以外）は `error` |
| `myon.ffi.free_callback` | `myon.ffi.free_callback(ptr: int) ret bool, error` | 登録済みコールバックを解放しスロットを再利用可能にする |

最大4引数、整数/ポインタ系、int64戻り値、同時16スロットのコールバックに対応します。
double 引数や外部スレッドからの呼び出しには対応しません。
静的トランポリンからツリーウォークまたは MVM の関数を呼びます。
関数内の実行時エラーは診断を表示して0を返し、後続の C 呼び出しを継続します。
C 側が参照している間は free_callback しないでください。

##### SDL の例

`examples/ffi_sdl_event_loop.myon` は alloc したブロックを SDL_PollEvent の out 引数へ
渡し、read_i32 でフィールドを取得します。以前の文書で示した定数は次のとおりです。

| 定数 | 値 | 意味 |
| ---- | -- | ---- |
| `SDL_QUIT` | `256` (`0x100`) | ウィンドウを閉じる操作等 |
| `SDL_KEYDOWN` | `768` (`0x300`) | キー押下 |
| `SDL_KEYUP` | `769` (`0x301`) | キー解放 |
| `SDL_MOUSEMOTION` | `1024` (`0x400`) | マウス移動 |
| `SDL_MOUSEBUTTONDOWN` | `1025` (`0x401`) | マウスボタン押下 |
| `SDL_MOUSEBUTTONUP` | `1026` (`0x402`) | マウスボタン解放 |

| offset | フィールド | 説明 |
| ------ | ---------- | ---- |
| 0  | `Uint32 type` | イベント種別（上表の定数） |
| 4  | `Uint32 timestamp` | タイムスタンプ |
| 8  | `Uint32 windowID` | ウィンドウ ID |
| 12 | `Uint8 state` / `Uint8 repeat` | 押下(1)/解放(0)・リピート（KEYDOWN/KEYUP の type でも判別可能） |
| 16 | `SDL_Scancode scancode` | 物理キー位置を表すスキャンコード |
| 20 | `SDL_Keycode sym` | 論理キーコード（多くのキーで ASCII 近似。例: `SDLK_a` = 97） |

オフセットの表は SDL2 / x86-64 の例で、実際のレイアウトを検査する代わりにはなりません。
サイズは使用する SDL のヘッダとビルド条件で確認してください。大きめのバッファを確保する
だけでは、レイアウトの一致や外部関数のアクセス境界は確認できません。

SDL_mixer のオーディオ例は `examples/ffi_sdl_audio.myon` にあります。
SDL_Init、Mix_OpenAudio、Mix_LoadWAV_RW/Mix_LoadMUS、再生、解放、終了の順に
資源を管理します。音源と SDL2/SDL_mixer の共有ライブラリが必要です。

### 10.4 数学と文字列

#### 数値関数の戻り値型

abs/max/min/clamp/mod は入力の型に応じて int/float を使います。
floor/ceil/round/trunc/sign は int、sqrt/pow/三角関数/対数は float を返します。
戻り値型は「float入力があれば必ずfloat」という単一の規則ではありません。

最小 int の符号反転・abs、表現不能な gcd は実行時エラーです。
intへの変換や丸め結果の変換は、有限性と int64 範囲を確認します。
lcm/pow_int は範囲外を error 戻り値で報告します。

#### myon.math

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `sqrt` | `(x: float) ret float` | 平方根（常にfloat） |
| `pow` | `(a: float, b: float) ret float` | べき乗（常にfloat） |
| `abs` | `(x: int\|float) ret int\|float` | 絶対値（型保持） |
| `floor` | `(x: int\|float) ret int` | 床関数（常にint、int入力はそのまま） |
| `ceil` | `(x: int\|float) ret int` | 天井関数（常にint、int入力はそのまま） |
| `round` | `(x: int\|float) ret int` | 四捨五入（C標準`round()`、常にint） |
| `trunc` | `(x: int\|float) ret int` | 0方向への切り捨て（常にint） |
| `max` | `(a: int\|float, b: int\|float) ret int\|float` | 最大値（型保持） |
| `min` | `(a: int\|float, b: int\|float) ret int\|float` | 最小値（型保持） |
| `sin` `cos` `tan` | `(x: float) ret float` | 三角関数（ラジアン） |
| `asin` `acos` `atan` | `(x: float) ret float` | 逆三角関数 |
| `atan2` | `(y: float, x: float) ret float` | 2引数逆正接 |
| `log` | `(x: float) ret float` | 自然対数 |
| `log2` | `(x: float) ret float` | 底2の対数 |
| `log10` | `(x: float) ret float` | 常用対数 |
| `exp` | `(x: float) ret float` | 指数関数 |
| `mod` | `(a: int\|float, b: int\|float) ret int\|float, error` | 剰余。ゼロ除算時は`error`を返す（型保持） |
| `sign` | `(x: int\|float) ret int` | 符号（正=1 / 負=-1 / ゼロ=0、常にint） |
| `clamp` | `(x, lo, hi: int\|float) ret int\|float` | lo以上hi以下にクランプ（3つ全てintならint演算） |
| `gcd` | `(a: int, b: int) ret int` | 最大公約数（ユークリッドの互除法）。負値は絶対値扱い、常に非負を返す。`gcd(0,0)`は`0` |
| `lcm` | `(a: int, b: int) ret int, error` | 最小公倍数。`a=0`または`b=0`は`0`。int64をオーバーフローする場合は`error` |
| `pow_int` | `(base: int, exp: int) ret int, error` | 整数のべき乗を繰り返し二乗法でint64のまま正確に計算。`exp<0`は`error`（分数になるため。浮動小数点が必要なら`pow`を使う）、オーバーフロー時も`error` |
| `pi` | `() ret float` | 円周率 π |
| `e` | `() ret float` | ネイピア数 e |

#### myon.string

length はバイト数、length_chars は Unicode コードポイント数です。
substring/index_of と空区切り split はコードポイント単位で動きます。
結合文字や絵文字列の書記素クラスタ単位ではありません。
upper/lower はUnicode 17.0.0のDefault Case Conversion（full mapping）を使います。
`ß → SS` のような展開とΣの文脈依存変換に対応し、OSのロケールには依存しません。
不正UTF-8は元のバイトを保持し、文脈の区切りにします。言語別tailoringや正規化は行いません。
データと再生成手順は [Unicode大小文字変換](unicode.md) を参照してください。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `length` | `(s: str) ret int` | バイト数（`strlen`相当） |
| `length_chars` | `(s: str) ret int` | 文字数（Unicodeコードポイント数） |
| `concat` | `(a: str, b: str) ret str` | 連結 |
| `contains` | `(s: str, sub: str) ret bool` | 部分文字列を含むか |
| `upper` / `lower` | `(s: str) ret str` | 大文字化 / 小文字化（Unicode 17.0.0、言語別tailoringなし） |
| `substring` | `(s: str, start: int, len: int) ret str, error` | start文字目からlen文字分（文字数ベース）。範囲外は`error` |
| `split` | `(s: str, sep: str) ret myon.array(str)` | sep区切りで分割。空sepは1文字ずつ、空sは空配列 |
| `join` | `(parts: myon.array(str), sep: str) ret str` | sepで連結。空配列は`""` |
| `trim` | `(s: str) ret str` | 先頭・末尾のASCII空白を除去 |
| `replace` | `(s: str, from: str, to: str) ret str` | fromを全てtoに置換。空fromはそのまま返す |
| `index_of` | `(s: str, sub: str) ret int` | subの最初の出現位置（文字数、0-indexed）。無ければ`-1` |
| `starts_with` | `(s: str, prefix: str) ret bool` | 接頭辞判定 |
| `ends_with` | `(s: str, suffix: str) ret bool` | 接尾辞判定 |
| `repeat` | `(s: str, n: int) ret str, error` | sをn回繰り返す。n<0は`error`、n=0は`""` |
| `to_int` | `(s: str) ret int, error` | 整数へパース（`strtoll`）。失敗時は`error` |
| `to_float` | `(s: str) ret float, error` | 浮動小数点数へパース（`strtod`）。失敗時は`error` |
| `from_int` | `(n: int) ret str` | 整数を文字列化 |
| `from_float` | `(f: float) ret str` | 浮動小数点数を文字列化（15〜17桁から往復を確認して選択。下記参照） |

```myon
module myon.stdio
module myon.string
myon.print(myon.string.length("あ"))
myon.print(myon.string.length_chars("あ"))
part, err = myon.string.substring("こんにちは", 1, 3)
myon.print(part)
```

#### float の表示

float の文字列化は %.15g、%.16g、%.17g の順に試し、strtod で元の値に戻る最初の
桁数を採用します。これは実装が用いる15〜17桁の選択手順です。
すべての形式を探索して文字数の最短性を証明するアルゴリズムという意味ではありません。

小数点も指数もない有限値には `.0` を付けます。特別な値は nan/inf/-inf と表示します。
value_to_cstr を共有する両エンジンに適用されます。検査例は p_float_roundtrip です。

### 10.5 時刻

now/now_ms は UNIX エポックからの秒/ミリ秒を int で返します。
壁時計時刻なので、経過時間測定に単調増加を仮定しないでください。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `now` | `() ret int` | 現在時刻（UNIXエポック秒）。C標準の`time(NULL)`相当 |
| `now_ms` | `() ret int` | 現在時刻（UNIXエポックミリ秒）。`clock_gettime(CLOCK_REALTIME)`相当 |
| `sleep_ms` | `(ms: int) ret void` | msミリ秒スリープ（`nanosleep`）。`ms<=0`は何もせず即座に返す |
| `frame_start` | `() ret int` | フレーム開始時刻を記録する。実装は`now_ms()`と同じで、意味を明確にするためのラッパー |
| `frame_wait` | `(frame_start_ms: int, target_fps: int) ret int` | ゲームループ用のFPS制御。`frame_start`で記録した時刻から現在までの経過時間を求め、`1000/target_fps`ミリ秒に満たない分だけスリープする。戻り値は実際に経過した時間（スリープ込みのdt、ミリ秒）。`target_fps<=0`はスリープせず経過時間だけを返す（FPS制限なし） |

frame_wait は目標1フレーム長 `1000/target_fps` に満たない残り時間だけ待ちます。
OS のスケジューリングにより実際の間隔は変動します。
コルーチン内の sleep はイベントループへ制御を返し、それ以外ではブロックします。

### 10.6 乱数

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `seed` | `(n: int) ret void` | 乱数生成器を`srand((unsigned)n)`で初期化する |
| `int` | `(lo: int, hi: int) ret int, error` | lo以上hi以下（両端含む）の一様乱数。`lo>hi`は`error` |
| `float` | `() ret float` | 0.0以上1.0未満の一様乱数 |
| `secure_int` | `(lo: int, hi: int) ret int, error` | OS の CSPRNG を使った lo以上hi以下（両端含む）の乱数。`lo>hi` は `error` |

seed/int/float は C の srand/rand を使います。seed の省略時は初回に時刻で初期化します。
同じ実装の同じ seed は再現用に使えますが、OS・Cライブラリをまたいだ同一列は規定しません。
広い範囲の int は分布に偏りが生じる場合があり、暗号用途向けの系統ではありません。

secure_int は OS の乱数源と棄却サンプリングを使い、seed に依存しません。
macOS/BSD の arc4random_buf、対応する POSIX の /dev/urandom を使用します。
乱数源が利用できない環境では error を返し、rand へ切り替えません。

### 10.7 IPv4 ソケット

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `tcp_socket` | `() ret int, error` | TCPソケットを生成しIDを返す。失敗時 `-1` + error |
| `udp_socket` | `() ret int, error` | UDPソケットを生成しIDを返す。失敗時 `-1` + error |
| `bind` | `(id: int, host: str, port: int) ret bool, error` | 指定アドレス/ポートにバインド。`host=""` は `INADDR_ANY` |
| `listen` | `(id: int, backlog: int) ret bool, error` | 接続待ち受けを開始する |
| `local_port` | `(id: int) ret int, error` | `bind(port=0)` 後などの実際のローカルポート番号を返す |
| `accept` | `(id: int) ret int, error` | 接続を受理し、新しい接続ソケットのIDを返す（協調的に待機） |
| `connect` | `(id: int, host: str, port: int) ret bool, error` | 相手へ接続する（非ブロッキング接続を協調的に待機）。`host` はIPアドレスの他、ホスト名も指定可（下記DNS参照） |
| `send` | `(id: int, data: str) ret int, error` | 送信したバイト数を返す |
| `recv` | `(id: int, maxlen: int) ret str, error` | 最大 `maxlen` バイト受信した文字列を返す（`maxlen<=0` は4096） |
| `send_to` | `(id: int, data: str, host: str, port: int) ret int, error` | UDP宛先指定送信 |
| `recv_from` | `(id: int, maxlen: int) ret str, str, error` | UDP受信。**3値タプル** `(data, "host:port", error)` |
| `close` | `(id: int) ret bool, error` | ソケットを閉じる（常に成功扱い） |

整数結果の失敗は -1 と error、真偽値の失敗は false と error を返す規約です。
recv_from はデータ・送信元・error の3値です。str で扱うデータには NUL 終端の制約があります。

ノンブロッキング I/O の待ちは、コルーチン内ではイベントループへ制御を返します。
外側では同期の待機処理を使用します。ホスト名は getaddrinfo で IPv4 に解決し、
最初の候補を使います。IPv6 と複数アドレスの順次接続は未対応です。

POSIX はソケット API、Windows は Winsock2 を使用します。
公開 fd 型は intptr_t 相当の myon_fd_t で、Windows の SOCKET 幅を保持します。
Windows の初期化・終了、エラー番号、closesocket 等の差は net.c で扱います。

### 10.8 HTTP と HTTPS

HTTP/1.0 のクライアントとサーバーを提供します。接続ごとに1リクエストを扱い、
Keep-Alive と TLS サーバーには対応しません。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `serve_static` | `(port: int, root: str) ret bool, error` | `root` 以下の静的ファイルを配信するサーバーを起動 |
| `serve` | `(port: int, handler: func) ret bool, error` | リクエストごとに `handler` を呼ぶ動的サーバーを起動 |
| `get` | `(url: str) ret str, int, error` | GETリクエスト。**3値** `(body, status, error)` |
| `post` | `(url: str, body: str, content_type: str) ret str, int, error` | POSTリクエスト。**3値** `(body, status, error)` |

serve は method/path/body の3文字列をハンドラへ渡します。
ハンドラが文字列を返す場合は 200 / text/plain の応答ボディです。
配列を返す場合は末尾をボディ、先頭をステータス、要素が3つ以上なら2番目を Content-Type として
解釈します。ステータスは100〜599です。文字列型の配列では例えば次のように指定できます。

```myon
module myon.http
module myon.array
myon.func handler(method: str, path: str, body: str) ret myon.array(str) {
    response = myon.array(str)
    response.push("404")
    response.push("text/plain")
    response.push("not found")
    ret response
}
```

サーバーを起動する呼び出しは `myon.http.serve(8080, handler)` です。
この呼び出しをトップレベルで実行すると待機し続けます。
コルーチン内では受理ループ等をデーモンとして扱い、プログラム終了時にそれだけを理由として
待機を続けないようにします。接続処理は別タスクとして実行します。

静的配信は .. や制御文字を含む要求パスを拒否し、/ や末尾スラッシュを index.html に対応させます。
これだけで配信元の全ファイル・symlink の権限境界を定めるわけではありません。

get/post は body/status/error の3値を返します。URL・接続などの失敗は空ボディとstatus=0とerrorです。
HTTPS の既定ポートは443、HTTPは80です。クライアントの TLS は OpenSSL を使います。

| 項目 | 内容 |
|------|------|
| 証明書チェーン検証 | `SSL_VERIFY_PEER`。システム既定の信頼ストア（`SSL_CTX_set_default_verify_paths`）に対して検証する。検証失敗時はハンドシェイクを中断 |
| ホスト名照合 | DNS 名は `SSL_set1_host()`、IP リテラルは `X509_VERIFY_PARAM_set1_ip_asc()`（証明書の iPAddress SAN）で照合 |
| 最低プロトコル版 | TLS 1.2（`TLS1_2_VERSION`）。TLS 1.0/1.1 は拒否（RFC 8996） |
| SNI | DNS 名のときのみ送信。IP リテラルでは送らない（RFC 6066） |
| 無効化するレガシー機能 | TLS 圧縮・再ネゴシエーション（`SSL_OP_NO_COMPRESSION \| SSL_OP_NO_RENEGOTIATION`） |
| ハンドシェイクのタイムアウト | 既定 30 秒（`MYON_TLS_HANDSHAKE_TIMEOUT_SECS`）。無応答のピアで無限待ちしない |

証明書チェーンや接続先名の検証に失敗した場合は接続を中止します。
証明書の有効期間の検査と失効確認は別です。OCSP/CRLによる失効確認、ピンニング、
クライアント証明書は未対応です。
URLのホスト・ポート・パスに含まれる制御文字と範囲外ポートは拒否します。

## 11. モジュール

組み込みには myon.stdio/math/string/file/array/map/time/random/net/http/ffi があります。

| 分類 | 記法 | 解決 |
|---|---|---|
| 組み込み | `module myon.stdio` | 登録済みの標準ライブラリ名 |
| 外部ファイル | `module external.util as util` | 実行スクリプト相対の util.myon |
| パッケージ | `module example.tools as tools` | ロック内の module 名前空間と配置済みソース |

パッケージは alias が必要です。module名とパッケージ名・リポジトリ名を区別します。
組み込み関数には対応するmoduleの宣言が必要です。入出力は `myon.stdio`、
`myon.math.*` などは各名前空間の宣言を要求します。`myon.array(...)` / `myon.map(...)` の
明示的な生成にはそれぞれ `myon.array` / `myon.map` を宣言します。
スカラー型変換・制御構文・型注釈・既存コレクションのメソッドはこの条件に含めません。

トップレベルの組み込み宣言は、本文と外部取り込みの実行に先立って有効になります。
重複宣言は無害で、宣言の効果はインタプリタ内で累積します。REPLでは次の入力へ引き継ぎ、
取り込んだモジュールの宣言も同じセッションへ共有します。新しいインタプリタでは初期化します。
ブロック・関数内でmodule宣言を実行するとエラーです。宣言の検査は引数の評価前に行います。
これはAPIの利用条件であり、ファイル・ネットワークなどの権限分離ではありません。
external の取り込みはツリーウォーク専用です。パッケージは MVM でも共通ローダーを使用し、
本体をツリーウォークで実行して関数・値を接続します。

形式・解決規則は [パッケージマネージャー](package_manager.md)、
配布手順は [パッケージ開発](package_development.md) を参照してください。

## 12. 実行と対話モード

トップレベルの文を順に実行し、専用の main 関数は要求しません。

### 12.0 CLI

```sh
myon program.myon
myon --compile program.myon -o program.myc
myon program.myc
myon --run-mvm program.myon
myon --dump-bytecode program.myc
myon --tokens program.myon
myon --tokens -
myon --help
myon --version
```

--compile は出力のみで実行しません。先頭がMYC1または拡張子が.mycならVMへ渡します。
それ以外はソースで、`-` は標準入力です。
保存元のソースとの差を検出すると警告し、--strict-staleではエラーにします。
元ソースがなければ検査を省略します。詳細は MVM リファレンス6.5節を参照してください。

### 12.1 REPL

引数なしで起動します。myon> が通常、...> が入力継続のプロンプトです。
定義はセッション中保持します。実行時エラー後も次の入力を受け付け、exit/quit/EOFで終了します。
REPL はツリーウォーク実行です。

## 13. 構文の概要

次の EBNF は主要な構文の対応を示します。改行の扱いは1.4節、型・取り込みの制約は
各節を参照してください。トークン単位の受理規則は src/parser.c が実装しています。

```ebnf
program       = { statement, { separator } } ;
separator     = newline | ";" ;
statement     = system_decl | module_decl | assignment | expression
              | if_stmt | while_stmt | for_stmt | func_decl | struct_decl
              | "myon.break" | "myon.continue" | return_stmt
              | "myon.expose", identifier | block ;
block         = "{", { statement, { separator } }, "}" ;
system_decl   = "system", "myon.useversion", "=", int_literal ;
module_decl   = "module", module_path, [ "as", identifier ] ;
module_path   = identifier, { ".", identifier } ;
assignment    = identifier, { ",", identifier }, [ ":", type ], "=", expression
              | identifier, compound_op, expression
              | postfix_target, "=", expression ;
compound_op   = "+=" | "-=" | "*=" | "/=" ;
if_stmt       = "myon.if", expression, [ "then" ], block,
                { "myon.elif", expression, "then", block }, [ "myon.else", block ] ;
while_stmt    = "myon.while", expression, block ;
for_stmt      = "myon.for", identifier, "myon.in", ( range_expr | expression ), block ;
range_expr    = "range", "(", expression, ",", expression, ")" ;
func_decl     = [ "myon.async" ], "myon.func", identifier, [ type_params ],
                "(", [ params ], ")", "ret", return_types, block ;
params        = identifier, ":", type, { ",", identifier, ":", type } ;
return_types  = type, { ",", type } ;
return_stmt   = "ret", [ expression, { ",", expression } ] ;
struct_decl   = "myon.struct", identifier, [ type_params ],
                [ "myon.extends", identifier ], "{", { field | func_decl | separator }, "}" ;
field         = identifier, ":", type ;
type_params   = "<", identifier, { ",", identifier }, ">" ;
expression    = or_expr ;
or_expr       = and_expr, { "myon.or", and_expr } ;
and_expr      = not_expr, { "myon.and", not_expr } ;
not_expr      = { "myon.not" }, comparison ;
comparison    = additive, { comp_op, additive } ;
comp_op       = "==" | "!=" | "<" | ">" | "<=" | ">=" ;
additive      = multiplicative, { ( "+" | "-" ), multiplicative } ;
multiplicative = unary, { ( "*" | "/" ), unary } ;
unary         = { "-" }, postfix ;
postfix       = primary, { "(", [ args ], ")" | "[", expression, "]" | ".", identifier } ;
postfix_target = identifier, ( "[", expression, "]" | ".", identifier ),
                 { "[", expression, "]" | ".", identifier } ;
args          = arg, { ",", arg } ;
arg           = [ identifier, "=" ], expression ;
primary       = literal | identifier, [ "<", type, { ",", type }, ">" ]
              | "(", expression, ")" | "myon.await", expression | lambda
              | "myon.array", "(", type, ")"
              | "myon.map", "(", type, ",", type, ")"
              | constructor, "(", expression, ")" ;
constructor   = "int" | "str" | "char" | "error" ;
lambda        = "myon.lambda", "(", [ params ], ")", "ret", return_types, block ;
type          = "int" | "float" | "str" | "char" | "bool" | "void" | "error"
              | identifier, [ "<", type, { ",", type }, ">" ]
              | "myon.array", "(", type, ")" | "myon.map", "(", type, ",", type, ")" ;
literal       = int_literal | float_literal | string_literal | "true" | "false" | "myon.nil" ;
```

int_literal / float_literal / string_literal / identifier の字句規則は1節を参照してください。
組み込みの専用トークンと修飾名は、説明上この概要の識別子に対応させています。

## 14. コレクション・型パラメータ・非同期処理

### 14.1 数値リテラル

10進、0x/0Xの16進、0o/0Oの8進、指数表記を扱います。
int の範囲を超えた整数リテラルはパース時に拒否します。
最小intを算術で作る場合は `-9223372036854775807 - 1` のように書けます。

### 14.2 Map

```myon
module myon.map
module myon.stdio
m = myon.map(str, int)
m.set("age", 20)
myon.print(m.get("age"))
```

| メソッド | シグネチャ | 説明 |
|------|-----------|------|
| `set` | `(k: K, v: V) ret void` | キー`k`に値`v`を設定 |
| `get` | `(k: K) ret V` | キー`k`の値（無ければ`myon.nil`） |
| `has` | `(k: K) ret bool` | キー`k`が存在するか |
| `delete` | `(k: K) ret bool` | キー`k`を削除（削除できたか） |
| `keys` | `() ret myon.array(K)` | 全キーを要素型`K`の配列で返す |
| `values` | `() ret myon.array(V)` | 全値を要素型`V`の配列で返す |
| `length` | `() ret int` | エントリ数 |

キー型は str/int を想定したAPIです。比較は従来どおり型を区別する `value_equal` を使います。
ハッシュ索引による検索・更新・削除は平均O(1)、拡張時の再ハッシュはO(n)、lengthはO(1)です。
衝突が集中した場合はO(n)になります。文字列キーのハッシュ計算と比較はバイト長に比例します。
keys/valuesとMapの表示は挿入順です。既存キーの更新は位置を維持し、削除後の再追加は末尾になります。
keysとvaluesは対応する順序で返します。

### 14.3 関数内の制御フロー

ret で関数から戻ります。任意位置への goto 構文はありません。
ループ内では break/continue を使います。

### 14.4 標準ライブラリの参照

標準入出力、ファイル、FFI、math/string、time/random、net/http の関数表は10節にあります。
配列は7.1節、Mapは14.2節を参照してください。

### 14.5 循環取り込み

外部ファイル・パッケージの module 取り込みで循環を検出するとエラーを報告します。
外部パッケージの依存解決時の循環と、実行時の module 取り込みの循環は別の検査です。

### 14.6 継承のフィールド名

子構造体が親と同名のフィールドを宣言することは拒否します。
フィールドとメソッドの解決は、それぞれの宣言と継承チェーンを使います。

### 14.7 型注釈と評価結果

関数の引数・構造体フィールドには型を記述します。
変数の型注釈を省いた場合は評価結果を使います。
型情報があることと、すべての代入経路がコンパイル時に検査されることは同義ではありません。

### 14.8 ジェネリクス

```myon
module myon.array
module myon.stdio
myon.struct Box<T> { value: T }
box = Box<int>(value=5)
myon.print(box.value)

myon.func first<T>(xs: myon.array(T)) ret T {
    ret xs[0]
}
```

型パラメータへ型を割り当てます。T: Comparable のような型制約構文はありません。
MVM は型消去でコンパイルし、型引数ごとの単相化は行いません。

### 14.9 async と await

```myon
module myon.time
module myon.stdio
myon.async myon.func fetch() ret str {
    myon.await myon.time.sleep_ms(10)
    ret str("done")
}
task = fetch()
myon.print(myon.await task)
```

async 関数の呼び出しは Task を返します。本体はイベントループで実行され、await は
完了を待って結果を取得します。通常の値に対する await はその値を返します。
Task内の失敗は await 側でも実行時エラーとして報告します。

処理は単一スレッドで、タスクは await・タイマー・I/O 待ちで協調的に中断します。
CPU処理を一定時間で中断するプリエンプションや、マルチコア並列実行は行いません。
POSIX は ucontext、Windows は Win32 Fiber を使用します。

各コルーチンのスタック予算は256 KiBです。再帰深度の上限はこのスタックとビルド条件から
計算します。mainのスタック上限とは別です。完了タスクのスタック/Fiberはスケジューラへ
戻ってから解放し、結果のメタデータはハンドルと待機タスクの参照がある間保持します。

WindowsのFiberエントリはreturnせず、完了を記録してmain_fiberへ切り替えます。
別のFiberへ戻った後でDeleteFiberを呼びます。イベントループ破棄時だけに解放する方式ではありません。
未完了タスクのコンテキストはループ破棄時にも解放します。詳細と検証範囲は既知の問題13を参照してください。

## 15. 現在の制約と検討対象

未対応なのは IPv6、TLSサーバー、HTTP Keep-Alive、MVMのexternal取り込みとREPLなどです。
対応範囲と不具合の修正記録は既知一覧で管理します。
マルチスレッド化や追加の型制約は、現実装の仕様ではありません。

HTTPハンドラのステータス/Content-Type指定、HTTPSクライアント、クロージャ、
asyncのMVM対応は既に実装されており、未実装項目とは区別してください。

## 16. 実行例

```myon
system myon.useversion=1
module myon.stdio

module myon.array
myon.struct Person {
    name: str
    myon.func greet() ret void {
        myon.print("Hello, {self.name}")
    }
}

myon.func divide(a: int, b: int) ret int, error {
    myon.if b == 0 then { ret 0, error("division by zero") }
    ret a / b, myon.nil
}

person = Person(name=str("myon"))
person.greet()
result, err = divide(10, 2)
myon.if err == myon.nil then { myon.print(result) }

numbers = myon.array(int)
numbers.push(1)
numbers.push(2)
myon.for item myon.in numbers {
    myon.print(item)
}
```

この例はツリーウォーク、または宣言ASTを保持する --run-mvm で実行できます。
構造体を含む保存済み.mycの制約は MVM リファレンス7節を参照してください。
