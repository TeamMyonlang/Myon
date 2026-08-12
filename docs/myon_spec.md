# Myon言語仕様書 v0.1

## 0. 概要

**Myon** は実用性を見据えた、型に厳密なプログラミング言語である。
`myon.` プレフィックスによる名前空間の明示、`str()`/`char()`/`int()` のような値コンストラクタ風の型構文、
そして独自のスコープ公開機構 `myon.expose` を特徴とする。

### (0.1and)0.2 設計方針まとめ

| 項目 | 方針 |
|---|---|
| プレフィックス | `myon.stdio` 等モジュール読み込み以外は原則 `myon.` を付与 |
| 型システム | 厳密（異なる型同士の演算はエラー） |
| コメント | `#`（改行不可） / `//`（改行可） / `/* */`（複数行） |
| 実装言語 | C（理想はアセンブリ） |
| エラー処理 | Rust/Go風、戻り値にエラーを含める |
| null | `myon.nil` はエラー値専用。通常変数はnil不可・初期化必須 |
| 整数オーバーフロー | 実行時エラー |

---

## 1. 字句規則

### 1.1 コメント

```myon
# 単一行コメント（改行不可）
// 単一行コメント（改行可能、JS的）
/* 
複数行コメント
何行でも書ける
*/
```

### 1.2 識別子

- 使用可能文字：英数字とアンダースコア（`_`）
- 数字始まりは不可
- 大文字・小文字を区別する

### 1.3 リテラル

| 型 | 例 | 備考 |
|---|---|---|
| int | `1`, `42`, `0x1F`, `0o17` | 10進数・16進数・8進数（14.1節参照） |
| float | `3.14`, `1.5e10` | 通常表記・指数表記（14.1節参照） |
| str | `str("人間")` | コンストラクタ経由 |
| char | `char("a")` | コンストラクタ経由、1文字 |
| bool | `true`, `false` | 直接代入可（int同様の扱い） |

### 1.4 文の区切り

- 改行で文を区切るのが基本
- セミコロン `;` を使えば1行に複数文を書ける

```myon
x = 1; y = 2; myon.print(x + y)
```

- ただし `()` / `[]` の内部にある改行は文の区切りとみなさない。
  これにより、関数呼び出しの引数リストや配列アクセスなどを複数行に
  分けて書ける（字句解析器が `(`・`[` の深さを数え、深度が0より大きい
  間は改行を読み飛ばす）。

```myon
hero = Hero(
    name = str("勇者"),
    hp = 80
)
```

- 一方、ブロック本体を囲む `{}` の内部では改行は引き続き文の区切りとして
  機能する（複数の文を並べるために改行が必要なため、`{}` は上記の深さ計算に
  含めない）。

### 1.5 予約語一覧

```
system module myon ret str char int float bool void error
myon.if myon.elif myon.else then
myon.while myon.for myon.in myon.break myon.continue range
myon.func myon.struct myon.array myon.map myon.print myon.input
myon.and myon.or myon.not myon.nil myon.expose
myon.lambda myon.extends self myon.async myon.await
true false as
```

---

## 2. 型システム

### 2.1 プリミティブ型

- `int`：整数。オーバーフロー時は実行時エラー
- `float`：浮動小数点数
- `str`：文字列（`str(...)` で構築）
- `char`：1文字（`char(...)` で構築）
- `bool`：真偽値（`true`/`false` を直接代入可）

### 2.2 型の厳密性

異なる型同士の演算は**すべてエラー**となる（例：`char + int` はコンパイル/実行時エラー）。

### 2.3 型変換（キャスト）

型コンストラクタが変換も兼ねる。

```myon
n = int("123")     // str → int
s = str(123)        // int → str
c = char(65)         // int → char
```

### 2.4 myon.nil

`myon.nil` は**エラー値専用**。通常の変数（int/str/bool等）に代入することはできず、
すべての変数は宣言時に初期化が必須。

```myon
x: int          // エラー：初期化なしの宣言は不可
x = 0            // OK

result, err = divide(10, 0)
myon.if err != myon.nil then { ... }   // myon.nilはerror型の文脈でのみ有効
```

---

## 3. 変数と代入

```myon
x = str("人間")
a = char("a")
b = 1
flag = true
```

### 3.1 演算子

**算術・比較**
```
+  -  *  /
==  !=  <  >  <=  >=
```

**論理演算子（プレフィックススタイル）**
```
myon.and  myon.or  myon.not
```

`myon.not` は前置専用の単項演算子である（EBNF: `not_expr = [ "myon.not" ], comparison ;`）。
必ずオペランドの前に置くこと。後置で書くと構文エラーになる。

```myon
// 正しい書き方（前置）
myon.if myon.not flag then { ... }

// 誤り（後置は構文エラーになる）
// myon.if flag myon.not then { ... }
```

**複合代入演算子**
```
+=  -=  *=  /=
```

### 3.2 演算子優先順位（上ほど優先度が高い）

| 優先度 | 演算子 | 種別 |
|---|---|---|
| 1 | `()` `[]` `.` | グループ化・添字・メンバアクセス |
| 2 | `myon.not` `-`（単項） | 単項演算子 |
| 3 | `*` `/` | 乗除 |
| 4 | `+` `-` | 加減 |
| 5 | `==` `!=` `<` `>` `<=` `>=` | 比較 |
| 6 | `myon.and` | 論理AND |
| 7 | `myon.or` | 論理OR |

---

## 4. 文字列補間

`{}` 内に任意の式を書ける（変数参照・計算・プロパティアクセス等）。

```myon
myon.print("Hello, {name}! Next year you'll be {age + 1}.")
myon.print("{p.name}'s age is {p.age}.")
```

---

## 5. 制御構文

### 5.1 条件分岐

```myon
myon.if a > b {
    myon.print("aがbよりデカい")
} myon.elif b > a then {
    myon.print("bがaよりデカい")
} myon.else {
    myon.print("しらね")
}
```

`then` は `myon.elif` の文法要素として残す（`myon.if`/`myon.else` には不要）。

### 5.2 繰り返し

```myon
myon.while a < b {
    myon.print(a)
    a = a + 1
    myon.if a == 5 then { myon.break }
}

myon.for i myon.in range(0, 10) {
    myon.if i == 3 then { myon.continue }
    myon.print(i)
}

myon.for item myon.in xs {
    myon.print(item)
}
```

---

## 6. 関数

### 6.1 基本構文

引数の型注釈は必須。戻り値は `ret 型` の後置スタイル。voidは明示的に `ret void`。

```myon
myon.func add(a: int, b: int) ret int {
    ret a + b
}

myon.func greet(name: str) ret void {
    myon.print("Hello, ", name, "!")
}
```

### 6.2 複数戻り値とエラー処理（Rust/Go風）

`error` はエラー専用の型で、`error(str)` の形でエラー値を生成する組み込みコンストラクタを持つ
（`str()`/`char()`/`int()` と同様の値コンストラクタパターン）。

```myon
myon.func divide(a: int, b: int) ret int, error {
    myon.if b == 0 then {
        ret 0, error("ゼロ除算")
    }
    ret a / b, myon.nil
}

result, err = divide(10, 0)
myon.if err != myon.nil then {
    myon.print("エラー: ", err)
} myon.else {
    myon.print("結果: ", result)
}
```

### 6.2.1 診断メッセージの形式（構文/実行時エラー）

構文エラー・実行時エラーは、gcc / Rust コンパイラ風の詳細な形式で報告する。

- **行番号と列番号**の両方を表示する（字句解析器が保持する `col` を活用）。
- **該当行のソースコード抜粋**と、エラー位置を指す `^` マーカーを表示する。
- 構文エラーでは、期待していたトークン種別を人間が読みやすい表現に変換して出す
  （例：`TOK_RPAREN` ではなく `')'`、`TOK_EOF` ではなく `end of file`）。

```
myon: syntax error at line 5, column 17: expected ')' to close call (got an integer literal)
     5 | myon.print(1, 2 3)
       |                 ^
```

実行時エラー（型不一致・範囲外アクセス・ゼロ除算・整数オーバーフロー等）でも、
発生行のソース抜粋を表示する。実行時は列情報を保持していないため、キャレットは
行頭を指すベストエフォートの位置となる。

```
myon: runtime error at line 4: division by zero
     4 | myon.print(a / b)
       | ^
```

### 6.3 ラムダ（無名関数）

`myon.lambda` で定義。外側スコープの変数を捕捉できる（通常のクロージャ）。

```myon
threshold = 10
isOver = myon.lambda(x: int) ret bool { ret x > threshold }
myon.print(isOver(15))   // true
```

---

## 7. 配列

`myon.array` 型として定義。可変長・同一型要素のみ。

```myon
xs = myon.array(int)
xs.push(1)
xs.push(2)
xs.push(3)

y = xs[0]              // []記法でアクセス
xs.pop()                 // 末尾を削除
len = xs.length()        // 要素数取得
xs.push("文字列")        // エラー：型不一致
```

### 7.1 配列メソッド（Phase4）

`push` / `pop` / `length` に加え、並べ替え・検索・部分取得・高階関数の
メソッドを備える。`sort` / `sort_desc` / `reverse` は**破壊的**（呼び出し
元の配列をその場で書き換える）で、それ以外は元の配列を変更せず新しい
配列や値を返す。

| メソッド | シグネチャ | 説明 |
|------|-----------|------|
| `push` | `(v: T) ret void` | 末尾に追加（型不一致は`error`） |
| `pop` | `() ret T` | 末尾を削除して返す |
| `length` | `() ret int` | 要素数 |
| `sort` | `() ret void` | 昇順にその場でソート（破壊的）。要素は全て`int`/`float`/`str`である必要がある（混在した`int`/`float`は`double`比較、`str`は`strcmp`のバイト列辞書順）。それ以外の型が混ざると`runtime_error` |
| `sort_desc` | `() ret void` | 降順にその場でソート（破壊的）。比較ロジックは`sort`と同じ |
| `reverse` | `() ret void` | 要素順をその場で反転（破壊的） |
| `contains` | `(v) ret bool` | `v`と等しい要素が存在するか（`value_equal`） |
| `index_of` | `(v) ret int` | `v`と最初に等しい要素の位置（0-indexed）。無ければ`-1` |
| `slice` | `(start: int, len: int) ret myon.array(T), error` | `start`番目から`len`個を取り出した新しい配列。`start`/`len`が負、または`start+len`が配列長超過なら`error` |
| `map` | `(f) ret myon.array(?)` | 各要素に1引数関数/ラムダ`f`を適用した新しい配列。要素型は先頭要素の変換結果から推定（空配列は型無し） |
| `filter` | `(f) ret myon.array(T)` | `f`の結果がtruthyな要素だけを残した新しい配列（要素型は元と同じ） |
| `reduce` | `(f, init) ret ?` | `acc=init`から左畳み込み（各要素で`acc = f(acc, elem)`）。空配列は`init`をそのまま返す |

`map` / `filter` / `reduce` は `myon.lambda` を引数に取れる高階関数である。

```myon
xs = myon.array(int)
xs.push(3)
xs.push(1)
xs.push(2)
xs.sort()                       // [1, 2, 3]（破壊的）
xs.sort_desc()                  // [3, 2, 1]
xs.reverse()                    // [1, 2, 3]
myon.print(xs.contains(2))      // true
myon.print(xs.index_of(2))      // 1
mid, merr = xs.slice(1, 2)      // [2, 3]（元の配列は不変）

doubled = xs.map(myon.lambda(x: int) ret int { ret x * 2 })       // [2, 4, 6]
sum = xs.reduce(myon.lambda(acc: int, x: int) ret int { ret acc + x }, 0)  // 6
```

---

## 8. 構造体（struct）

複数フィールド・異種混在データの受け皿。フィールド名指定でインスタンス生成。
メソッド定義・継承をサポートする。

```myon
myon.struct Person {
    name: str
    age: int

    myon.func greet() ret void {
        myon.print("Hello, my name is ", self.name)
    }
}

myon.struct Employee myon.extends Person {
    salary: int

    myon.func showSalary() ret void {
        myon.print(self.name, "'s salary: ", self.salary)
    }
}

p = Person(name=str("太郎"), age=20)
p.greet()

e = Employee(name=str("花子"), age=30, salary=5000)
e.greet()
e.showSalary()
```

---

## 9. スコープ規則

### 9.1 基本方針

ブロック（`{}`）スコープを基本としつつ、独自の `myon.expose` により
明示的に外側スコープへ変数を持ち出すことができる。

```myon
x = 1
{
    y = 2
    myon.expose y
}
myon.print(y)   // OK: exposeされたので見える
myon.print(x)   // OK: 元々外側
```

`myon.expose` を使わない限り、ブロックを抜けた変数は消える。

### 9.2 シャドーイング

**禁止**。同名変数の再定義はエラーとなる。

```myon
x = 1
{
    x = 2   // エラー：同名変数の再定義
}
```

このシャドーイング禁止ルールと代入の解決規則は、単純代入
（`x = ...`）だけでなく**多重代入（`x, y = ...`）にも同じように
適用される**。すなわち、

1. 代入先の名前が**現在のスコープ**に既に束縛されていれば、その束縛を
   更新する。
2. 現在のスコープには無いが**外側のスコープ**に束縛が存在する場合、
   - 現在のスコープが明示的な `{ }` ブロックであれば、外側変数の
     シャドーイングとみなしエラーとなる（上記の禁止ルール）。
   - 関数本体などブロックでないスコープであれば、外側の既存の束縛を
     更新する。
3. どこにも束縛が無ければ、現在のスコープに新規定義する。

したがって、関数内で `x, y = pair()` のように多重代入を行った場合、
`x` や `y` が外側スコープの既存変数であれば、（新しいローカル変数を
作るのではなく）その外側の変数が更新される。

```myon
x = 100
y = 200
myon.func inner() ret void {
    x, y = pair()   // 外側の x, y を更新する（新規ローカルは作らない）
}
inner()
myon.print(x)       // pair() の第1返り値
myon.print(y)       // pair() の第2返り値
```

---

## 10. 入出力

### 10.1 標準入出力

```myon
name = myon.input("お名前は？")   // str型で受け取る
myon.print("Hello Worlddd! ", x + "!")
```

### 10.2 ファイルI/O

`module myon.stdio` を宣言すると、以下のファイル操作関数が利用できる。
読み書きに失敗した場合（ファイル不在・権限不足など）は、`runtime_error` で
プロセスを終了させるのではなく、6.2節のエラー処理方式（Rust/Go 風、`error(...)`）
に従って第2戻り値として `error` 値を返す。呼び出し側は `myon.if err != myon.nil`
でこれを捕捉できる。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `myon.file.read`   | `myon.file.read(path: str) ret str, error`            | ファイル全体を `str` として読み込む |
| `myon.file.write`  | `myon.file.write(path: str, content: str) ret bool, error`  | 上書き書き込み（成功時 `true`） |
| `myon.file.append` | `myon.file.append(path: str, content: str) ret bool, error` | 末尾に追記（成功時 `true`） |
| `myon.file.exists` | `myon.file.exists(path: str) ret bool`                | 存在確認（エラーは返さない） |

```myon
module myon.stdio

path = "/tmp/greeting.txt"
ok, werr = myon.file.write(path, "line1")
myon.if werr != myon.nil then { myon.print("書き込み失敗") }

aok, aerr = myon.file.append(path, "-line2")

content, rerr = myon.file.read(path)
myon.if rerr != myon.nil then {
    myon.print("読み込み失敗")
} myon.else {
    myon.print(content)          // line1-line2
}

myon.print(myon.file.exists(path))                       // true
myon.print(myon.file.exists("/tmp/does_not_exist.txt"))  // false
```

- 成功時の第2戻り値（`error`）は `myon.nil` になる。
- `NULL` パス等、明らかに未定義動作になりうる入力はガードする。
  パストラバーサル対策など高度な安全性は本フェーズの対象外。

---

### 10.3 C FFI（外部共有ライブラリ呼び出し, Phase3）

`module myon.ffi` を宣言すると、ビルド済みの共有ライブラリ
（Linux: `.so` / macOS: `.dylib` / Windows: `.dll`）に含まれるC関数を
実行時に読み込んで直接呼び出せる。内部的には `dlopen`/`dlsym`/`dlclose`
相当の仕組みを使う（`libffi` などの外部依存は持たない）。

失敗（ライブラリ不在・シンボル未検出・型不一致など）は 6.2 節の
エラー処理方式に従い、第2戻り値として `error` 値を返す。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `myon.ffi.load`   | `myon.ffi.load(path: str) ret int, error`             | 共有ライブラリを読み込み、ハンドルID（`int`）を返す |
| `myon.ffi.close`  | `myon.ffi.close(handle: int) ret bool, error`         | ハンドルを閉じる（成功時 `true`） |
| `myon.ffi.call_i` | `myon.ffi.call_i(handle: int, name: str, sig: str, ...) ret int, error`   | `int` を返すC関数を呼ぶ |
| `myon.ffi.call_d` | `myon.ffi.call_d(handle: int, name: str, sig: str, ...) ret float, error` | `double` を返すC関数を呼ぶ |
| `myon.ffi.call_p` | `myon.ffi.call_p(handle: int, name: str, sig: str, ...) ret int, error`   | ポインタを返すC関数を呼ぶ（戻り値は `int` として表現） |
| `myon.ffi.call_v` | `myon.ffi.call_v(handle: int, name: str, sig: str, ...) ret bool, error`  | `void`（戻り値なし）のC関数を呼ぶ（成功時 `true`） |

**シグネチャ文字列（`sig`）の記法**

引数の型を1文字ずつ並べた文字列で指定する。

| 文字 | 対応するC型 | Myon側の型 |
|------|-------------|-----------|
| `i`  | `long long`（整数）  | `int` / `bool` |
| `d`  | `double`             | `float`（`int` も可、自動的に `double` へ変換） |
| `p`  | `void*`（不透明ポインタ） | `int`（ポインタ値をそのまま整数として保持） |
| `s`  | `const char*`        | `str` |
| `b`  | `void*`（Myonが確保したメモリブロックを指す, **Phase3.1**） | `int`（`myon.ffi.alloc` が返すブロックID） |

`'b'` と `'p'` の違い（Phase3.1）:

- `'p'` は「Cが返してきた生アドレス値をそのまま `int64` として受け渡すだけ」で、
  Myon側はそのブロックの中身を一切関知しない。
- `'b'` は「Myon側の `myon.ffi.alloc` で確保したブロックIDを渡すと、C呼び出しの
  直前にFFI層がそのブロックの実際のメモリアドレスに変換してからC関数に渡す」。
  これにより `sqlite3_open(path, &db)` のような「出力引数（ダブルポインタ）」に、
  Myonが確保した書き込み可能な領域のアドレスを安全に渡せる。
  `'b'` に `myon.ffi.alloc`（や `load`）が発行していない不正な値を渡した場合は
  `error` を返す（クラッシュを防ぐガード）。
- 引数の並び順の制約において、`'b'` は `'i'`/`'p'`/`'s'` と同じ「整数系」グループに
  属する（`double` 引数より前に置く）。

例: `"id"` は「`int` 引数を1つ、`double` 引数を1つ」の意味。
戻り値の型は関数名で分ける（`call_i` / `call_d` / `call_p` / `call_v`）。
引数は0〜6個までサポートする。`sig` が空文字列（`str("")`）の場合は
引数0個（可変長引数を一切渡さない）を意味する。

**対応する値の型と制約**

- 対応する値の型は `int64` / `double` / `ptr` / `string` の4種類のみ。
  構造体を値渡し・値返しする関数は本フェーズでは非対応。
- ポインタを介した受け渡しはサポートするが、呼び出し先が構造体の中身を
  書き込んで返す関数（例: `SDL_PollEvent(SDL_Event *event)`）は、その
  構造体メモリをMyon側で `ptr` として確保できないため扱えない。
  そのため本フェーズのFFIだけでは、ウィンドウを出して一定時間後に閉じる
  程度のデモは組めるが、`SDL_PollEvent` を使う一般的なイベントループ形式の
  GUIアプリは組めない。
  （この制約は Phase3 時点のもの。Phase3.1 のメモリ確保／型付き読み出しと
  Phase4.1 の型付き書き込み・構造体DSL、および Phase5.1 で追加した
  `myon.ffi.read_i32` によって、確保済みブロックを `'b'` で渡し `SDL_Event`
  のフィールドを読み戻す形で out 方向の構造体読み取りが可能になった。
  詳細は後述の「(5) SDL_Event の読み取り（Phase5.1）」を参照。）
- 引数の並びは「整数系（`i`/`p`/`s`）の引数がすべて先、`double`（`d`）の
  引数がすべて後」の順序のみサポートする（x86-64 System V ABI の
  レジスタ割り当てに対応。SDL2 の主要関数はこの並びに収まる）。
  それ以外の並び順や7引数以上は `error` を返す。
- **32bit幅のC引数への注意**: Myonの `int` は64bit だが、呼び出し先の
  C関数の引数が `int` / `unsigned int` / `Uint32` のような32bit幅である
  場合がある。呼ばれた側が下位32bitのみを参照するため、`0 〜 4294967295`
  の範囲で値を渡す限り問題は起きない。この範囲を超える値や負の値を渡すと
  C関数が意図しない値を受け取る可能性があるため、呼び出し側が正しい範囲の
  値を渡す責任を持つ（本フェーズでは自動的な範囲チェックは行わない）。

**対応プラットフォーム**

Linux と Windows で本実装済み。

- **Linux**: `dlopen`/`dlsym`/`dlclose` を用いて `.so` を読み込む。
- **Windows**: `LoadLibraryA`/`GetProcAddress`/`FreeLibrary`（`kernel32`）を
  用いて `.dll` を読み込む。読み込み失敗時のエラーメッセージは
  `GetLastError()` + `FormatMessageA`（`FORMAT_MESSAGE_FROM_SYSTEM |
  FORMAT_MESSAGE_IGNORE_INSERTS`）で人間可読な文字列へ変換される。
  なお、拡張子（`.dll`）の自動補完は行わないため、呼び出し側が
  `myon.ffi.load(str("xxx.dll"))` のようにパスを明示する
  （Windows の DLL 検索順序については各自の配布形態に従うこと）。

macOS では引き続きスタブで、`myon.ffi.load` を呼んだ時点で
「FFI is not supported on macOS yet (Phase3 stub)」という `error` を返す
（コンパイル自体は3OSとも通る）。

```myon
module myon.ffi

handle, err = myon.ffi.load(str("libm.so.6"))
myon.if err != myon.nil then { myon.print(str("load failed")) }

// sqrt(16.0) -> 4.0
result, cerr = myon.ffi.call_d(handle, str("sqrt"), str("d"), 16.0)
myon.print(result)                                 // 4

// pow(2.0, 10.0) -> 1024.0
p, perr = myon.ffi.call_d(handle, str("pow"), str("dd"), 2.0, 10.0)
myon.print(p)                                      // 1024

ok, clerr = myon.ffi.close(handle)
```

- 一度 `close` したハンドルIDは再利用されない。閉じ済みハンドルへの
  再アクセス（再 `close` や `call_*`）は `error` を返す。

#### 10.3.1 メモリ確保・文字列・バイト列（Phase3.1）

Phase3のFFIだけでは、(1) 文字列を返すC関数の中身を読めない、(2) 出力引数
（ダブルポインタ）を取るC関数にNULLしか渡せずクラッシュする、(3) 長さ付きの
任意バイト列を安全に受け渡せない、といった制約があった。Phase3.1では以下を
追加してこれらに対応する。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `myon.ffi.alloc`       | `myon.ffi.alloc(size: int) ret int, error`               | `size` バイトのゼロ初期化済みメモリを確保し、非負のブロックID（`int`）を返す。`size` が0以下なら `error` |
| `myon.ffi.free`        | `myon.ffi.free(block_id: int) ret bool, error`           | ブロックを解放する（成功時 `true`）。無効／解放済みIDは `error` |
| `myon.ffi.read_cstr`   | `myon.ffi.read_cstr(addr: int, max_len: int) ret str, error` | 生アドレス `addr` が指すNUL終端C文字列を `str` として読み出す |
| `myon.ffi.write_bytes` | `myon.ffi.write_bytes(block_id: int, offset: int, data: str) ret bool, error` | ブロック内の `offset` から `data` のバイト列を書き込む（成功時 `true`）。範囲外は `error` |
| `myon.ffi.read_bytes`  | `myon.ffi.read_bytes(block_id: int, offset: int, len: int) ret str, error`    | ブロックの `[offset, offset+len)` を `str` として読み出す。範囲外は `error` |
| `myon.ffi.read_i64`    | `myon.ffi.read_i64(block_id: int, offset: int) ret int, error`                | ブロックの `offset` から8バイトをリトルエンディアンの `int` として読み出す。範囲外は `error` |
| `myon.ffi.read_i32`    | `myon.ffi.read_i32(block_id: int, offset: int) ret int, error`                | ブロックの `offset` から4バイトをリトルエンディアンの int32 として読み出し、符号拡張して `int` を返す（`read_i64` の4バイト版。Phase5.1で追加）。範囲外は `error` |

**ブロックIDと生アドレスは別の名前空間**

`myon.ffi.alloc` が返す「ブロックID」は、MyonのFFI層が内部で管理する番号で
あり、Cから見た実際のメモリアドレスとは異なる（`myon.ffi.load` のハンドルIDと
同じ考え方）。Myonスクリプト側に生ポインタは一切公開されない。C関数の引数として
メモリのアドレスを渡したい場面では、sig文字に `'b'` を使うことで、FFI層が
呼び出し直前にブロックID→実アドレスへ変換する。

一方、`call_p` の戻り値や `read_cstr` の第1引数 `addr` は「Cの生アドレス値」で
あり、ブロックIDではない。

**`myon.ffi.read_cstr` の詳細**

- `max_len` で読み取る最大バイト数を制限する（暴走読み取り防止）。上限は
  16MB（16777216）で、これを超える値や0以下の値を指定すると `error` を返す。
- `addr` が `0`（NULL相当）の場合は `error` を返す（クラッシュしない）。
- ただし `0` 以外の**不正な**アドレスを渡した場合はクラッシュしうる。
  本フェーズでは signal handler 等による保護は行わない（呼び出し側が
  `call_*` から得た正しいアドレスを渡す責任を持つ）。

```myon
module myon.ffi

handle, err = myon.ffi.load(str("libz.so.1"))

# const char *zlibVersion(void) -> call_p で生アドレスを得る
verptr, e1 = myon.ffi.call_p(handle, str("zlibVersion"), str(""))

# 生アドレスを Myon の str にデリファレンス（最大64バイト）
version_str, e2 = myon.ffi.read_cstr(verptr, 64)
myon.print(version_str)                 # 例: "1.3.1"

ok, clerr = myon.ffi.close(handle)
```

**`myon.ffi.write_bytes` / `myon.ffi.read_bytes` の詳細**

`myon.ffi.alloc` で確保したブロックは「バイト列」の実体として扱える。バイト列は
「ブロックID + 有効データ長」という組で表現し、有効データ長は呼び出し側が別途
`int` で管理する。`write_bytes` / `read_bytes` は、ブロックIDを介してこの領域の
任意のオフセットに読み書きする。範囲外アクセス（`offset+len` がブロックサイズを
超える）になる場合は `error` を返す（メモリ破壊を防ぐガード）。

> **既知の制約（str型のバイナリ安全性）**
> MyonのstrはNUL終端の `char*` として表現され、明示的な長さフィールドを持たない。
> そのため `write_bytes` は `data` の `strlen()` ぶんのバイトを書き込み、途中に
> NULバイト（`0x00`）が含まれると、そこで途切れる。`read_bytes` の結果も同様に、
> 読み出したバイト列にNULが含まれると、可視の文字列がそこで途切れる（ベスト
> エフォート）。任意のバイナリ（NULを含むデータ）を厳密に扱う用途には、将来の
> フェーズで専用のバイト列型を追加する余地を残している。

```myon
module myon.ffi

handle, err = myon.ffi.load(str("libz.so.1"))

# 16バイトのブロックを確保し、"hello"（5バイト）を書き込む
buf, aerr = myon.ffi.alloc(16)
w, we = myon.ffi.write_bytes(buf, 0, str("hello"))

# uLong crc32(uLong crc, const Bytef *buf, uInt len)
# sig "ibi": crc(int) + バッファ('b') + len(int)
crc, ce = myon.ffi.call_i(handle, str("crc32"), str("ibi"), 0, buf, 5)
myon.print(crc)                         # 907060870 (== zlib.crc32(b"hello"))

freed, ferr = myon.ffi.free(buf)
ok, clerr = myon.ffi.close(handle)
```

**出力引数（ダブルポインタ）の呼び出し例**

`myon.ffi.alloc` で確保したブロックを sig `'b'` で渡すと、C関数がそのブロックに
値を書き込む「出力引数」パターンを安全に扱える。書き込まれたポインタ値（8バイト）
は `myon.ffi.read_i64` でリトルエンディアンの `int`（生アドレス値）として読み出し、
続くC呼び出しに `'p'` で渡せる（x86-64のリトルエンディアンを前提とする）。

```myon
module myon.ffi

handle, err = myon.ffi.load(str("libsqlite3.so.0"))
# sqlite3 *db を受け取る8バイト領域（x86-64のポインタは8バイト）
outblock, aerr = myon.ffi.alloc(8)

# int sqlite3_open(const char *filename, sqlite3 **ppDb)
# sig "sb": 文字列引数 + バッファ（ブロックID）引数
rc, oerr = myon.ffi.call_i(handle, str("sqlite3_open"),
                           str("sb"), str(":memory:"), outblock)
# rc == 0 (SQLITE_OK) を期待。

# 書き込まれた db ハンドル（sqlite3*）を生アドレス値として取り出す
db_addr, re = myon.ffi.read_i64(outblock, 0)

# int sqlite3_close(sqlite3 *db) — db_addr は生アドレスなので 'p' で渡す
crc, cerr = myon.ffi.call_i(handle, str("sqlite3_close"), str("p"), db_addr)

freed, ferr = myon.ffi.free(outblock)
ok, clerr = myon.ffi.close(handle)
```

**Phase3.1の既知の制約（まとめ）**

- `'b'` 以外の生アドレス（`'p'` や `call_p` の戻り値）を `read_cstr` /
  `read_bytes` / `read_i64` の対象に使うこと自体は許容するが、`read_cstr` は
  生アドレスを直接デリファレンスするため、不正なアドレスを渡すとクラッシュ
  しうる（本フェーズでは signal handler 等による保護は行わない）。一方
  `read_bytes` / `read_i64` はブロックID経由でのみ動作し、範囲チェックを
  行うため安全である。
- str型はバイナリセーフではない（上記「str型のバイナリ安全性」を参照）。
- 出力引数を持つC関数（例: `sqlite3_open`）は、`'b'` で確保済みブロックの
  アドレスを渡すことで安全に呼び出せる。書き込まれたポインタ値は
  `read_i64` で取り出す。

（`examples/ffi_zlib_version.myon`、`examples/ffi_sqlite_open.myon` に
動作するサンプルがある。）

---

#### 10.3.2 GUI/ゲーム制作向け FFI 拡張（Phase4.1）

Phase3.1 までの「`int`/`double`/`ptr`/`string` の単純な値渡し ＋
バイト列 `write_bytes`/`read_bytes`」だけでは、SDL/OpenGL のような
GUI・ゲームライブラリを実用的に叩くには不足していた。Phase4.1 では
次の4系統を追加する。いずれも C 側のヘッダは一切読まず、Myon 側が
「このレイアウト・この型として扱う」と申告した内容をそのまま信じる
（Phase3 以来の大方針を踏襲）。

##### (1) 型付きメモリ書き込み（Step1）

`write_bytes` は Myon の `str`（NUL終端）をコピーするため、`int` を
エンコードした際にNULバイト（`0x00`）が混ざると途中で書き込みが
切れてしまい、`SDL_Rect` のような「Myon側で値を組み立ててCへ渡す」
構造体が作れなかった。型付き書き込みは値の生バイト列を
リトルエンディアンで直接書き込むため、NULバイトを含む値でも欠落なく
書ける。`read_i64`（Phase3.1）と対になる書き込み側 API である。

| 関数 | シグネチャ | 説明 |
| ---- | ---------- | ---- |
| `myon.ffi.write_i64` | `myon.ffi.write_i64(block_id: int, offset: int, v: int) ret bool, error` | `offset` に `v` を8バイト int64（LE）として書き込む |
| `myon.ffi.write_i32` | `myon.ffi.write_i32(block_id: int, offset: int, v: int) ret bool, error` | `offset` に `v` を4バイト int32（LE、上位32bitは切り捨て）として書き込む |
| `myon.ffi.write_f64` | `myon.ffi.write_f64(block_id: int, offset: int, v: float) ret bool, error` | `offset` に `v` を8バイト IEEE754 double として書き込む |
| `myon.ffi.write_f32` | `myon.ffi.write_f32(block_id: int, offset: int, v: float) ret bool, error` | `offset` に `v` を（double→float 変換後の）4バイト IEEE754 float として書き込む |

範囲外オフセットや無効なブロックID、値の型不一致（`i` 系に `float`、
`f` 系に `int` を渡す等）は `error` を返し、クラッシュしない。

```myon
module myon.ffi
module myon.stdio

id, e = myon.ffi.alloc(16)
myon.ffi.write_i32(id, 0, 100)     # 64 00 00 00
myon.ffi.write_i32(id, 4, 200)     # C8 00 00 00
v, ev = myon.ffi.read_i64(id, 0)   # 100 と 200 を結合した LE の値
myon.print(v)                      # 858993459300
myon.ffi.free(id)
```

##### (2) 構造体レイアウト DSL（Step2）

フィールド名（インデックス）と型のリストから、各フィールドの
オフセットと構造体全体のサイズを自動計算し、オフセットの手計算なしで
構造体ブロックへ読み書きできる薄い DSL。

- 対応フィールド型は `'i32'` / `'i64'` / `'f32'` / `'f64'` の4種のみ
  （型付き書き込みでカバーされる型に対応）。
- **アライメント**は一般的な C 構造体のデフォルト規則に従う。各
  フィールドはそのサイズの自然境界（4バイト型→4境界、8バイト型→8境界）
  に配置され、構造体全体のサイズは最大アライメント要求の倍数になるよう
  末尾がパディングされる。`SDL_Rect`（`int` ×4）ではパディングは
  発生しないが、`i32`/`i64` が混在する構造体では自動でパディングが入る。
- 定義は FFI 層内部の「名前→レイアウト」テーブルに保持され、同名の
  再定義は上書きされる。

| 関数 | シグネチャ | 説明 |
| ---- | ---------- | ---- |
| `myon.ffi.struct_def`   | `myon.ffi.struct_def(name: str, field_kinds: array of str) ret int, error` | `field_kinds`（例 `["i32","i32","i32","i32"]`）からレイアウトを定義し、合計サイズ（バイト数）を返す。不正な型文字列は `error` |
| `myon.ffi.struct_alloc` | `myon.ffi.struct_alloc(name: str) ret int, error` | 定義済み構造体のサイズ分を確保し、ブロックIDを返す糖衣関数。未定義名は `error` |
| `myon.ffi.struct_write` | `myon.ffi.struct_write(block_id: int, name: str, field_index: int, v: int\|float) ret bool, error` | `field_index` 番目のフィールドへ、その型に応じて書き込む。値の型とフィールド型が矛盾すれば `error` |
| `myon.ffi.struct_read`  | `myon.ffi.struct_read(block_id: int, name: str, field_index: int) ret int\|float, error` | 同フィールドを読み出す。戻り値のスカラー型（`int`/`float`）はフィールド型から実行時に決まる |

> **`struct_read` の戻り値型についての設計判断**
> 指示書では「戻り値型を実行時に決める案」と「`struct_read_i` /
> `struct_read_f` に分ける案」のいずれでもよいとされていた。Myon の
> タプル返却（`v, e = ...`）は内部的に型なしの配列（`make_result_pair`）
> にパックして返すため、フィールド型に応じて `int` 値・`float` 値の
> どちらを詰めても分割代入で自然にアンパックできる。関数を分けるより
> API が素直になるので、**単一の `struct_read` が実行時にスカラー型を
> 選ぶ設計**を採用した。

```myon
module myon.ffi
module myon.stdio

rf = myon.array(str)
rf.push(str("i32")); rf.push(str("i32")); rf.push(str("i32")); rf.push(str("i32"))
sz, e = myon.ffi.struct_def(str("SDL_Rect"), rf)   # 16

r, er = myon.ffi.struct_alloc(str("SDL_Rect"))
myon.ffi.struct_write(r, str("SDL_Rect"), 0, 50)   # x
myon.ffi.struct_write(r, str("SDL_Rect"), 1, 60)   # y
myon.ffi.struct_write(r, str("SDL_Rect"), 2, 200)  # w
myon.ffi.struct_write(r, str("SDL_Rect"), 3, 100)  # h
x, ex = myon.ffi.struct_read(r, str("SDL_Rect"), 0)
myon.print(x)                                      # 50
myon.ffi.free(r)
```

`i32`/`i64` 混在の例（`["i32","i64"]`）では、i32 がオフセット0、
i64 が（8境界へアライメントされて）オフセット8、合計サイズ16 となる。

##### (3) 配列一括読み書き（Step3）

頂点バッファ・色配列のような同型の値の連続を、Myon の `array` から
1回の呼び出しでブロックへ書き込む／読み出す。内部は単一値版の
ループなので特別な最適化はしていない（正しさを優先）。

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

`i32`/`f32` は要素あたり4バイト、`i64`/`f64` は8バイトを消費する。
`count`（または書き込む要素数）がブロックの範囲を超える場合は `error`。

```myon
module myon.ffi
module myon.stdio

id, e = myon.ffi.alloc(64)
xs = myon.array(int)
xs.push(1); xs.push(2); xs.push(3); xs.push(4)
myon.ffi.write_array_i32(id, 0, xs)
ri, eri = myon.ffi.read_array_i32(id, 0, 4)
myon.print(ri)             # [1, 2, 3, 4]
myon.ffi.free(id)
```

##### (4) コールバック関数ポインタ（限定版, Step4）

`SDL_SetEventFilter` のように「C ライブラリがコールバック関数ポインタを
受け取り後で呼び返す」パターンに、**限定的な形**で対応する。

**スコープ（この範囲を超えない）:**

- コールバックの引数は **最大4個**、型は **int64 または ptr（Cポインタ）
  相当のみ**（Cの `long long (*)(long long, ...)` に類する固定シグネチャ。
  `double` 引数のコールバックは非対応）。
- コールバックの戻り値は **int64 のみ**（`void` を返したい場合は戻り値を
  無視する運用でカバー）。
- 同時に登録できるコールバックは **固定上限16個**。

**実装方式:** libffi の closure 機構は使わない（依存を増やさないため）。
C 側にあらかじめ「スロット番号 × 引数個数（0〜4）」ぶんの静的な
トランポリン関数群を用意し、各トランポリンが自分のスロットに登録された
Myon 関数値を `call_function()` で呼び返す。シングルスレッド前提のため
スレッド安全性はスコープ外。コールバック内で `runtime_error` が起きても
プロセスは落とさず、`stderr` にメッセージを出して戻り値0を返す。

| 関数 | シグネチャ | 説明 |
| ---- | ---------- | ---- |
| `myon.ffi.make_callback` | `myon.ffi.make_callback(fn: func, arg_count: int) ret int, error` | Myon の関数値 `fn` をコールバックとして登録し、C から見た生の関数ポインタを `int`（アドレス値）で返す。この値はそのまま `'p'` 引数として `call_i`/`call_v` 等に渡せる。スロット枯渇や `arg_count` 範囲外（0〜4以外）は `error` |
| `myon.ffi.free_callback` | `myon.ffi.free_callback(ptr: int) ret bool, error` | 登録済みコールバックを解放しスロットを再利用可能にする |

```myon
module myon.ffi
module myon.stdio

printer = myon.lambda(x: int) ret int { myon.print(x) ret 0 }
h, e = myon.ffi.load(str("libfoo.so"))
cbp, ec = myon.ffi.make_callback(printer, 1)
# void call_twice(long long (*cb)(long long), long long x) { cb(x); cb(x+1); }
myon.ffi.call_v(h, str("call_twice"), str("pi"), cbp, 10)   # printer が 10, 11 で2回呼ばれる
myon.ffi.free_callback(cbp)
myon.ffi.close(h)
```

##### (5) SDL_Event の読み取り（out 方向構造体読み取り, Phase5.1）

ゲーム制作に必須の入力取得（キーボード・マウス・ウィンドウクローズ）は、
`int SDL_PollEvent(SDL_Event *event)` のように「C 関数が構造体メモリへ
書き込んで返す（out 引数）」パターンに依存する。Phase5.1 で追加した
`myon.ffi.read_i32`（+ Phase3.1 の `alloc` / `'b'` 引数）を組み合わせることで、
確保済みブロックを `SDL_Event*` として渡し、書き戻されたフィールドを
Myon 側で読み取れるようになった。

**基本的な流れ:**

1. `SDL_Event` ぶんのブロックを `myon.ffi.alloc` で確保する。`SDL_Event` は
   C の共用体（union）で、そのサイズは含まれる最大メンバのサイズになる。
   SDL2 では伝統的に 56 バイト前後だが、**バージョンやビルドオプションで
   変わりうる**ため、Myon 側は C ヘッダを読まない前提で「安全に大きめ」
   （例: 128 バイト）を確保しておくのが実践的な妥協策。
2. ブロックを `'b'` 引数として `SDL_PollEvent` に渡す（`call_i`, sig `"b"`）。
   戻り値は「イベントがあったか」を表す 0/1 の `int`。
3. 書き戻された `SDL_Event` の先頭 4 バイト（`Uint32 type`）を
   `myon.ffi.read_i32(block, 0)` で読み、イベント種別を判定する。

**主要なイベント種別の定数**（SDL2 `SDL_events.h`。値は固定）:

| 定数 | 値 | 意味 |
| ---- | -- | ---- |
| `SDL_QUIT` | `256` (`0x100`) | ウィンドウを閉じる操作等 |
| `SDL_KEYDOWN` | `768` (`0x300`) | キー押下 |
| `SDL_KEYUP` | `769` (`0x301`) | キー解放 |
| `SDL_MOUSEMOTION` | `1024` (`0x400`) | マウス移動 |
| `SDL_MOUSEBUTTONDOWN` | `1025` (`0x401`) | マウスボタン押下 |
| `SDL_MOUSEBUTTONUP` | `1026` (`0x402`) | マウスボタン解放 |

**`SDL_KeyboardEvent` の主なオフセット**（SDL2, x86-64 の一般的なレイアウト。
すべて `read_i32` で 4 バイト読み出す）:

| offset | フィールド | 説明 |
| ------ | ---------- | ---- |
| 0  | `Uint32 type` | イベント種別（上表の定数） |
| 4  | `Uint32 timestamp` | タイムスタンプ |
| 8  | `Uint32 windowID` | ウィンドウ ID |
| 12 | `Uint8 state` / `Uint8 repeat` | 押下(1)/解放(0)・リピート（KEYDOWN/KEYUP の type でも判別可能） |
| 16 | `SDL_Scancode scancode` | 物理キー位置を表すスキャンコード |
| 20 | `SDL_Keycode sym` | 論理キーコード（多くのキーで ASCII 近似。例: `SDLK_a` = 97） |

```myon
module myon.ffi
module myon.stdio

SDL_QUIT    = 256
SDL_KEYDOWN = 768

handle, err = myon.ffi.load(str("libSDL2-2.0.so.0"))
# SDL_Init(SDL_INIT_VIDEO) など（省略。examples/ffi_sdl_event_loop.myon 参照）

# SDL_Event ぶんのバッファ（安全のため 128 バイト確保）
event_block, aerr = myon.ffi.alloc(128)

running = true
myon.while running {
    # int SDL_PollEvent(SDL_Event *event) — sig "b" で確保済みブロックを渡す
    has_event, perr = myon.ffi.call_i(handle, str("SDL_PollEvent"),
                                      str("b"), event_block)
    myon.if has_event != 0 then {
        ev_type, terr = myon.ffi.read_i32(event_block, 0)   # 先頭4バイト = type
        myon.if ev_type == SDL_QUIT then {
            running = false
        } myon.elif ev_type == SDL_KEYDOWN then {
            sym, serr = myon.ffi.read_i32(event_block, 20)  # 論理キーコード
            myon.print(sym)
        }
    }
    myon.ffi.call_v(handle, str("SDL_Delay"), str("i"), 16)
}

myon.ffi.free(event_block)
```

**既知の制約:**

- `SDL_Event` の正確なレイアウト・サイズは **SDL2 のバージョンやビルド
  オプションによって変わりうる**。ここに示すオフセットは一般的な x86-64
  ビルドに基づく目安であり、環境によってはずれる可能性がある。ブロックを
  大きめに確保し、まず `type`（offset 0）が期待値になることを確認してから
  他フィールドを解釈すること。
- 全イベント種別（ゲームパッド、タッチ、テキスト入力等）のレイアウトを
  網羅しているわけではない。実用上必要となる `SDL_QUIT` /
  `SDL_KeyboardEvent`（およびマウス系）の読み取りを主眼とする。
- リトルエンディアン（x86-64 ネイティブ）を前提とする。

（動作するサンプルは `examples/ffi_sdl_event_loop.myon`。実際の入力操作が
ある `examples/snake5.myon` も同じパターンでイベントを読んでいる。）

##### (6) SDL_mixer によるオーディオ再生（Phase5.1）

効果音・BGM 再生は、専用のオーディオ・モジュールを新設せず、FFI 経由で
SDL_mixer（`libSDL2_mixer`）を叩くことで実現する。`libSDL2-2.0.so.0` と
`libSDL2_mixer-2.0.so.0` の 2 つを `myon.ffi.load` で読み込み、以下の流れで
再生する:

1. `SDL_Init(SDL_INIT_AUDIO)` でオーディオサブシステムを初期化する
   （`SDL_INIT_AUDIO = 0x10 = 16`）。
2. `int Mix_OpenAudio(int frequency, Uint16 format, int channels,
   int chunksize)` でミキサーを開く。一般的な設定値は
   `frequency=44100`、`format=MIX_DEFAULT_FORMAT`（`0x8010 = 32784`、
   リトルエンディアンでは `AUDIO_S16SYS` に相当）、`channels=2`、
   `chunksize=2048`。成功時 `0` を返す。sig は `"iiii"`。
3. `Mix_Chunk *Mix_LoadWAV(const char *file)` で WAV を読み込む（sig `"s"`、
   戻り値はポインタ handle なので `call_p`。失敗時は `0`/NULL）。
4. `int Mix_PlayChannel(int channel, Mix_Chunk *chunk, int loops)` で再生する
   （`channel=-1` は空きチャンネル、`loops=0` は 1 回再生。sig `"ipi"`）。
5. 後片付け: `Mix_FreeChunk(chunk)` → `Mix_CloseAudio()` → `SDL_Quit()` →
   `myon.ffi.close`。

**既知の制約:**

- `Mix_OpenAudio` は**利用可能なオーディオデバイスが無い環境（CI コンテナ、
  ヘッドレスサーバ等）では失敗する**（負値/非0を返す）。サンプルはこの失敗を
  検知して、クラッシュせずにエラーメッセージを表示して終了する作りにすること。
- `MIX_DEFAULT_FORMAT` の値は SDL_mixer のビルド（エンディアン）に依存する。
  上記 `0x8010` は一般的なリトルエンディアン x86-64 ビルドの値。

（動作するサンプルは `examples/ffi_sdl_audio.myon`。テスト用の小さな WAV
`examples/beep.wav` を同梱している。オーディオデバイスの無い環境では
`Mix_OpenAudio` の失敗を確認するに留まる。）

---

### 10.4 標準ライブラリ myon.math / myon.string（Phase3.5）

Step16で最小実装された `myon.math` / `myon.string` を、Phase3.5で実用的な
ラインナップに拡張・修正した。ここで確定した関数シグネチャが正式な仕様である
（14.4節・15章の「未確定」記述はこれをもって解消する）。

#### 数値の型保持ルール

`myon.math` の関数は、C言語などと同様の暗黙昇格ルールに従う。

- **全ての数値引数が `int` 型であれば結果も `int` 型**（数学的に整数を
  返しうる関数に限る）。
- **1つでも `float` が含まれていれば結果は `float` 型**。

「整数のままで意味が保たれる」関数（`abs` / `max` / `min` / `floor` /
`ceil` / `round` / `trunc` / `mod` / `sign` / `clamp`）は `int` 入力で
`int` を返し、`double` を経由しない。これにより `2^53` を超える `int64`
値でも丸め誤差なく正確に扱える。一方 `sqrt` / `pow` / 三角関数 / 対数などは
数学的に非整数を返しうるため、`int` 入力でも常に `float` を返す。

#### myon.math 関数一覧

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

`mod` はゼロ除算をスクリプト側でハンドリングできるよう、他の `myon.math`
関数と異なり `(value, error)` の2値返却にしている。

`gcd`（Phase4）は常に成功する単値返却だが、`lcm` / `pow_int`（Phase4）は
オーバーフロー・負指数を `error` として返すため `(value, error)` の
2値返却になる。`pow_int` は `pow` と異なり **double を経由しない** ため、
`2^62`（`4611686018427387904`）のような大きな指数の整数冪も桁落ちなく
正確に計算できる。

```myon
myon.print(myon.math.gcd(12, 18))                   // 6
myon.print(myon.math.gcd(-12, 18))                  // 6（絶対値扱い）
l, lerr = myon.math.lcm(4, 6)                        // 12
p, perr = myon.math.pow_int(2, 62)                   // 4611686018427387904
n, nerr = myon.math.pow_int(2, -1)                   // nerr != myon.nil（負指数）
o, oerr = myon.math.pow_int(2, 63)                   // oerr != myon.nil（int64オーバーフロー）
```

```myon
myon.print(myon.math.max(9223372036854775807, 1))  // 9223372036854775807（誤差なし）
myon.print(myon.math.floor(3.7))                    // 3
q, err = myon.math.mod(7, 0)                         // err != myon.nil（ゼロ除算）
myon.print(myon.math.clamp(15, 0, 10))              // 10
```

#### myon.string 関数一覧

`length` は **バイト数**（`strlen`相当、既存動作のまま）を返す。
Unicodeコードポイント数（文字数）が必要な場合は `length_chars` を使う。
`substring` / `index_of` / `split`（空区切り）
などのインデックス系関数は全て **文字数ベース** であり、バイトオフセット
ではない（日本語などのマルチバイト文字でも安全）。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `length` | `(s: str) ret int` | バイト数（`strlen`相当） |
| `length_chars` | `(s: str) ret int` | 文字数（Unicodeコードポイント数） |
| `concat` | `(a: str, b: str) ret str` | 連結 |
| `contains` | `(s: str, sub: str) ret bool` | 部分文字列を含むか |
| `upper` / `lower` | `(s: str) ret str` | 大文字化 / 小文字化（ASCII） |
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
| `from_float` | `(f: float) ret str` | 浮動小数点数を文字列化 |

```myon
myon.print(myon.string.length_chars(str("あ")))  // 1（文字数）
myon.print(myon.string.length(str("あ")))         // 3（バイト数）
sub, err = myon.string.substring(str("こんにちは"), 1, 3)  // "んにち"（文字数ベース）
myon.print(myon.string.index_of(str("こんにちは"), str("にち")))  // 2（文字数ベース）
parts = myon.string.split(str("a,b,c"), str(","))          // ["a", "b", "c"]
myon.print(myon.string.join(parts, str(",")))              // "a,b,c"
```

### 10.5 myon.time（Phase4）

現在時刻の取得とスリープのための最小限のモジュール。エポック時刻は
UNIXエポック（1970-01-01 UTC）からの経過時間で、いずれも `int`（int64）で
返す。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `now` | `() ret int` | 現在時刻（UNIXエポック秒）。C標準の`time(NULL)`相当 |
| `now_ms` | `() ret int` | 現在時刻（UNIXエポックミリ秒）。`clock_gettime(CLOCK_REALTIME)`相当 |
| `sleep_ms` | `(ms: int) ret void` | msミリ秒スリープ（`nanosleep`）。`ms<=0`は何もせず即座に返す |
| `frame_start` | `() ret int` | フレーム開始時刻を記録する（Phase5.1）。実装は`now_ms()`と同じで、意味を明確にするための糖衣関数 |
| `frame_wait` | `(frame_start_ms: int, target_fps: int) ret int` | ゲームループ用のFPS制御（Phase5.1）。`frame_start`で記録した時刻から現在までの経過時間を求め、`1000/target_fps`ミリ秒に満たない分だけスリープする。戻り値は実際に経過した時間（スリープ込みのdt、ミリ秒）。`target_fps<=0`はスリープせず経過時間だけを返す（FPS制限なし） |

`now_ms()` は `now() * 1000` とほぼ一致する（同一時刻の呼び出しなら
数ミリ秒程度の差）。POSIX環境（Linux/macOS）を前提とする。

```myon
module myon.time

t1 = myon.time.now()
myon.time.sleep_ms(1100)
t2 = myon.time.now()
myon.print(t2 - t1 >= 1)     // true（1秒以上経過）

myon.print(myon.time.now() > 0)      // true
myon.print(myon.time.now_ms() > 0)   // true
```

#### ゲームループヘルパー（Phase5.1）

`frame_start` / `frame_wait` は、毎フレームの経過時間（dt）計算と目標FPS
維持のためのスリープという定型処理を提供する。ゲームループの車輪の
再発明を避けるための薄いヘルパーである。`frame_wait` は `sleep_ms` と
同じスリープ経路を使うため、コルーチン内（`myon.async`/`myon.await`）で
呼び出した場合はプロセス全体をブロックせずイベントループに制御を譲る。

```myon
module myon.time
module myon.stdio

myon.while true {
    t0 = myon.time.frame_start()
    // ここでゲームの更新・描画処理
    dt = myon.time.frame_wait(t0, 60)  // 60FPSを目標
    // dt（ミリ秒）を物理演算等の時間ベース計算に使ってもよい
}
```

`target_fps` に `10` を指定すると、1フレームの目標時間は `1000/10 = 100`
ミリ秒となり、更新・描画処理が100msより短く終わった場合はその差分だけ
スリープして、ループの反復間隔がおおよそ100msに揃う。厳密なリアルタイム性は
OSのスケジューラに依存するため、多少の誤差は許容される。

### 10.6 myon.random（Phase4）

乱数生成のための最小限のモジュール。C標準の `srand` / `rand` を薄く
ラップしたものであり、**暗号学的に安全な乱数ではない**（`rand()`ベース）。
セキュリティ用途には使用しないこと。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `seed` | `(n: int) ret void` | 乱数生成器を`srand((unsigned)n)`で初期化する |
| `int` | `(lo: int, hi: int) ret int, error` | lo以上hi以下（両端含む）の一様乱数。`lo>hi`は`error` |
| `float` | `() ret float` | 0.0以上1.0未満の一様乱数 |

`seed()` を一度も呼ばずに `int()` / `float()` を呼んだ場合、初回のみ
`srand((unsigned)time(NULL))` 相当の自動初期化を行う（毎回シードし直すと
短時間の連続呼び出しで同じ値が返るため、初回だけ初期化する）。同じ値で
`seed()` すれば同じ乱数列が再現される（`srand`+`rand`の決定的性質に依存）。

`rand()` は `RAND_MAX` までの範囲しか返さないため、`hi-lo` が `RAND_MAX` を
超える広い範囲では分布に偏りが生じうる（このPhaseでは均一性は要求しない）。

```myon
module myon.random

myon.random.seed(42)
v, err = myon.random.int(1, 6)       // 1〜6のいずれか
myon.print(v >= 1)                    // true

f = myon.random.float()              // 0.0 <= f < 1.0
myon.print(f >= 0.0)                  // true

bad, berr = myon.random.int(5, 1)    // berr != myon.nil（lo > hi）
```

---

### 10.7 ネットワーク myon.net（Phase5）

IPv4 の TCP / UDP ソケットを直接扱うための低水準モジュール。POSIX
ソケットAPI（`socket`/`bind`/`listen`/`accept`/`connect`/`send`/`recv`/
`sendto`/`recvfrom`）の薄いラッパである。**Linux と Windows で本実装済み**で、
それ以外のプラットフォームでは全関数が即座に `error` を返す（後述）。
Windows では同等のソケット機能を **Winsock2**（`ws2_32`）で提供する
（下記「Windows 実装（Winsock2）」を参照）。

ソケットは整数の**ソケットID**で識別する。IDは内部テーブル（最大256個）の
インデックスであり、生のfdではない。全関数は非ブロッキングfdで動作し、
イベントループ上のコルーチン内から呼ばれた場合は自動的に協調的な待機
（イベントループへの譲渡）に切り替わる。トップレベル（コルーチン外）から
呼ばれた場合は単一fdの `select()` による同期待機にフォールバックする。

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

**エラー方針**：整数を返す関数（`tcp_socket`/`local_port`/`send` など）は
失敗時に `-1` と `error` を、`bool` を返す関数（`bind`/`listen`/`connect`）は
`false` と `error` を返す。`recv_from` のみ受信データ・送信元・エラーの
3値を返し、失敗時は空文字列2つと `error` を返す。

**非ブロッキング挙動**：内部的に would-block（`EAGAIN`/`EWOULDBLOCK`）は
`-2` として扱い、`recv`/`accept` は読み取り可能まで、`send`/`connect` は
書き込み可能まで、それぞれイベントループに譲渡して待機する。この待機は
`myon.async`/`myon.await` と組み合わせて複数接続を並行処理するための土台と
なる（→ 14.9）。

**DNS名前解決（Phase5.1）**：`connect`/`bind`/`send_to` の `host` 引数には、
IPv4アドレスの直打ち（例 `"127.0.0.1"`）に加えて**ホスト名**（例
`"example.com"` や `"localhost"`）も指定できる。内部的には、まず
`inet_pton()` でIPv4リテラルとして解釈を試み（高速パス）、解釈できない場合に
`getaddrinfo()` でDNS名前解決を行う（`AF_INET` に限定、ソケット種別に応じた
`SOCK_STREAM`/`SOCK_DGRAM` をヒントに指定）。複数の候補が返った場合は最初の
1つを使う（ラウンドロビン/フォールバックは未対応）。解決に失敗した場合は
`cannot resolve host '<name>': <理由>` の形の error を返す（理由の文字列は
プラットフォーム依存：Linux は `gai_strerror`、Windows は `WSAGetLastError`
を `FormatMessageA` で整形したもの）。

**Windows 実装（Winsock2）**：Windows では同じ関数群を Winsock2 で実装する。
POSIX 版との主な差異は以下の通り（挙動・戻り値の規約は Linux と同一）。

- **初期化/終了**：`net_state_create()` の初回呼び出しで
  `WSAStartup(MAKEWORD(2,2), ...)` を、最後の `net_state_destroy()` で
  `WSACleanup()` を呼ぶ。Winsock 自体が参照カウント式であることに加え、
  net.c 側でも初期化カウンタを持ち、多重初期化・早すぎる解放を防ぐ。
- **ソケット型**：ソケットは `int` ではなく `SOCKET`（`UINT_PTR`）で保持し、
  無効値は `-1` ではなく `INVALID_SOCKET`、失敗の戻り値は `SOCKET_ERROR`。
  Windows 版の `NetState` は `SOCKET socks[]` を持つ別定義（`net.h` の
  opaque 宣言により問題なし）。
- **非ブロッキング設定**：`fcntl(O_NONBLOCK)` の代わりに
  `ioctlsocket(s, FIONBIO, &mode)`（`mode=1`）を使う。
- **クローズ**：`close()` の代わりに `closesocket()`。
- **would-block 判定**：`EAGAIN`/`EWOULDBLOCK` の代わりに `WSAEWOULDBLOCK`
  を、非ブロッキング接続の進行中は `WSAEALREADY`/`WSAEINVAL` も `-2`（進行中）
  として扱う。接続完了検査は `getsockopt(SO_ERROR)` を用いる。
- **送受信長**：Winsock の `send`/`recv`/`sendto`/`recvfrom` は長さを `int`
  で受け取るため、内部で `INT_MAX` にクランプする。`MSG_NOSIGNAL` は
  Windows には無い（SIGPIPE が発生しない）ため指定しない。
- **エラー文字列**：`errno`/`strerror` の代わりに `WSAGetLastError()` の値を
  `FormatMessageA` で整形して `<prefix>: <text>` 形式の error を作る
  （FFI の Windows 実装 `ffi_platform.c` と同じ手法）。
- **リンク**：Windows ビルドでは `-lws2_32` を追加でリンクする（Makefile）。

**Step3（イベントループ）での fd 型の決着（Step2 申し送りの解消）**：
`net_raw_fd()` の戻り値型は `int` だが、Windows の `SOCKET` は
64bit 環境で `UINT_PTR`（64bit）である。Step3 で `event_loop.c` の Windows
版（Fiber + Winsock `select()`）を実装するにあたり、fd の型設計を次のように
**確定**した。

- **公開インターフェースの fd 型は全プラットフォームで `int` のまま維持する**
  （`event_loop.h` の `event_loop_wait_readable/writable(int fd)`、`net.h` の
  `net_raw_fd() ret int`）。これは Linux 側では fd がまさに `int` であり、
  ここを `intptr_t` 相当へ広げるのは Linux にとって無意味なインターフェース
  破壊になるため。また `interpreter.c` 内で fd を持ち回る多数の `int fd`
  ローカル変数への波及も避けられる。
- **Windows 側では `SOCKET` の再構成を局所化する**。`event_loop.c` と `net.c`
  の Windows 分岐で、Winsock `select()`/`fd_set` に渡す直前にのみ
  `(SOCKET)(UINT_PTR)(unsigned int)fd` で `int` → `SOCKET` を復元する。
  Winsock のカーネルハンドルは 32bit に収まる仕様（Microsoft Learn
  "Socket Handles"）であり、`(unsigned int)` によるゼロ拡張で元の
  `SOCKET` 値がロスなく復元される。したがって `int` 往復は実用上安全であり、
  切り詰めによる不具合は生じない。

**Windows 同期待機ヘルパ `net_sync_wait_fd(int fd, int for_write)`**：
コルーチン外（同期文脈）での単一 fd 待機は、従来 `interpreter.c` が
`select(2)` を直接呼んでいた。Windows では Winsock の `select()` が
`<winsock2.h>`（→ `<windows.h>` → `<winnt.h>`）を引き込み、`winnt.h` の
`TokenType` 列挙が Myon 自身の `TokenType`（`src/token.h`）と衝突する。
これを避けるため、Windows の同期単一 fd 待機は Winsock ヘッダを正しく内包
している `net.c` の `net_sync_wait_fd()` に委譲する。Linux では従来どおり
`interpreter.c` 内で `select(2)` を呼ぶ（`net_sync_wait_fd` の Linux 版は
対称性のために用意されているが未使用）。この設計により `interpreter.c` は
Windows でも `windows.h` を取り込まずにコンパイルできる。

**未サポート**：上記以外のプラットフォーム、IPv6。非対応プラットフォームでは
全関数が `myon.net unsupported on this platform` の error を返す。

```myon
module myon.net

// TCPエコーサーバー（1接続だけ受けて返す簡単な例）
srv, e1 = myon.net.tcp_socket()
myon.net.bind(srv, "", 0)              // ポート0で任意ポートにバインド
port, ep = myon.net.local_port(srv)    // 実際のポート番号を取得
myon.net.listen(srv, 16)

conn, e2 = myon.net.accept(srv)        // 接続を協調的に待機
data, e3 = myon.net.recv(conn, 1024)
myon.net.send(conn, data)              // エコー
myon.net.close(conn)
myon.net.close(srv)
```

---

### 10.8 HTTP myon.http（Phase5）

`myon.net` の上に構築された最小限の HTTP モジュール。サーバー2種と
クライアント2種を提供する。**HTTP/1.0・1コネクション1リクエスト固定**であり、
Keep-Alive は行わない（レスポンスに `Connection: close` を付与する）。

| 関数 | シグネチャ | 説明 |
|------|-----------|------|
| `serve_static` | `(port: int, root: str) ret bool, error` | `root` 以下の静的ファイルを配信するサーバーを起動 |
| `serve` | `(port: int, handler: func) ret bool, error` | リクエストごとに `handler` を呼ぶ動的サーバーを起動 |
| `get` | `(url: str) ret str, int, error` | GETリクエスト。**3値** `(body, status, error)` |
| `post` | `(url: str, body: str, content_type: str) ret str, int, error` | POSTリクエスト。**3値** `(body, status, error)` |

**サーバーの実行モデル**：`serve_static`/`serve` は受理ループ（accept loop）を
イベントループ上のコルーチンとして起動し、接続ごとにさらに別のコルーチンを
spawnして並行処理する。受理ループとそのラッパーは**デーモンタスク**として
マークされるため、プログラム終了時のドレイン処理では無視される。これにより
自己完結スクリプト（サーバーを起動し、別タスクでクライアント処理をして終了）
がハングせずに終了できる。トップレベルからサーバー関数を直接呼んだ場合は、
そのまま全タスク完了までブロックし続ける（常駐サーバー用途）。

**静的配信のパス解決**：`http_resolve_static_path` が `..` を含むパスや制御
バイトを含むパスを拒否し（ディレクトリトラバーサル防止）、`/` および末尾
スラッシュは `index.html` にマップする。

**ハンドラ規約**（`serve`）：`handler(method: str, path: str, body: str)` の形で
呼ばれ、返した文字列がレスポンスボディになる。現状のレスポンスは常に
`200 OK` / `Content-Type: text/plain` 固定である（ステータス・ヘッダの
カスタマイズは未対応、→ Open Questions）。

**クライアント**：`get`/`post` はlibcurl等に依存せず、生のTCP（`myon.net`）で
HTTP/1.0リクエストを自力で組み立てる自己完結実装である。戻り値はボディ・
ステータスコード・エラーの3値で、URLパースや接続に失敗した場合は空ボディ・
ステータス `0`・`error` を返す。ホスト名はDNS解決される（→ 10.7）。

**HTTPS/TLS（Phase5.1）**：`https://` で始まるURLに対応した。通常のTCP接続を
確立した上で、OpenSSL を用いたネイティブTLSハンドシェイクを行う（`src/tls.c`。
`SSL_CTX_new` → `SSL_new` → `SSL_set_fd` → `SSL_connect` → `SSL_read`/
`SSL_write` → `SSL_shutdown` の薄いラッパ）。SNI（Server Name Indication）と、
システムの既定証明書ストア（`SSL_CTX_set_default_verify_paths`）に対する
証明書検証・ホスト名照合（`SSL_set1_host`）をベストエフォートで有効にしている。
デフォルトポートは `https` が `443`、`http` が `80`。

> **ビルド依存**：この機能により、Myon本体のビルドに OpenSSL 開発パッケージ
> （`libssl-dev` 相当）が**必須**となった。Makefile は常に `-lssl -lcrypto` を
> リンクする。
>
> **セキュリティ上の注意**：TLSの証明書検証は簡略化されており、本実装は
> 堅牢化された（hardened）TLSクライアントではない。中間者攻撃に対して脆弱な
> 可能性があるため、信頼できないネットワーク上での機密情報の送受信には
> 使用しないこと。

```myon
module myon.http

// 静的ファイルサーバー（常駐）
myon.http.serve_static(8080, "./public")

// 動的ハンドラ
fn handler(method: str, path: str, body: str) ret str {
    ret "hello from " + path
}
myon.http.serve(8080, handler)

// クライアント
body, status, err = myon.http.get("http://127.0.0.1:8080/")
myon.print(status)                      // 200

// HTTPS（TLS）クライアント（Phase5.1）
body2, status2, err2 = myon.http.get("https://example.com/")
myon.print(status2)                     // 200
```

---

## 11. モジュールシステム

```myon
system myon.useversion=1     // 使用するMyonバージョンの宣言
module myon.stdio               // 組み込みモジュール
module external.util.math as m  // 外部モジュール（./util/math.myon）をmとして読み込み
```

ドット区切りはファイルパス階層に対応する（`.myon` 拡張子を想定）。

---

## 12. エントリーポイント

専用の `main` は不要。トップレベルの文が上から順に実行される。

```myon
system myon.useversion=1
module myon.stdio

x = str("人間")
myon.print("Hello Worlddd! ", x + "!")
```

### 12.0 コマンドラインインタフェース（CLI）

`myon` コマンドの引数体系は次の通り。フラグは `--tokens` のようなダブルハイフン
スタイルで統一する。

```sh
myon <file.myon>                 # .myon をツリーウォーク型インタプリタで実行
myon <file.myc>                  # .myc（MVM バイトコード）をバイトコード VM で実行
myon --compile <src> [-o out]    # .myon を MVM バイトコード（.myc）にコンパイル（実行しない）
myon --dump-bytecode <src>       # .myon（または .myc）を逆アセンブルして表示し終了
myon --tokens <file.myon>        # トークン列を出力して終了（Step 1 の確認用）
myon --tokens -                  # 標準入力からソースを読み、トークン列を出力
myon                             # 引数なし: 対話式実行（REPL）
myon -h | --help                 # 使い方を表示して終了
```

- **ファイル引数の振り分け**：先頭 4 バイトが MVM のマジック `MYC1`
  （`.myc` ヘッダ。定義は [`docs/mvm_spec.md`](mvm_spec.md) の 6.2 節）で
  あるか、名前が `.myc` で終わる場合はバイトコード VM で実行する。それ以外は
  従来通りツリーウォーク型インタプリタで実行する。`-` は常に標準入力を意味し、
  ソースとして扱う。**`.myon` 実行（拡張子指定なしを含む）の挙動は従来と一切
  変わらない。**
- **`-o <out>`**：`--compile` の出力先を指定する。省略時は入力名の拡張子を
  `.myc` に置き換えた名前（例：`foo.myon` → `foo.myc`）。
- **`--compile`** はコンパイルのみで実行はしない。
- 未知の `--` 付きフラグはエラー（終了コード 64）として扱い、使い方を表示する。
- **`--run-mvm <src>`**（内部確認用）：`.myon` をメモリ上でコンパイルし、そのまま
  MVM VM で実行する。`.myon`/`.myc` の等価性検証スイート
  （`tests/run_mvm_tests.sh`）が使う内部フラグで、通常の実行では不要。

### 12.1 対話モード（REPL）

`myon` を引数なしで起動すると対話式実行（REPL）モードに入る。

- プロンプトは `myon> `。入力が構文として未完（`()`/`[]`/`{}` が閉じていない等）の
  場合は継続プロンプト `...> ` を表示し、閉じるまで入力を受け付ける
- REPL 内で定義した変数・関数・構造体はセッション終了まで保持される
  （インタプリタと環境を使い回す）
- 実行時エラーが起きても REPL は終了せず、次の入力を受け付け続ける
  （エラーはそのステートメントの中断のみ）
- `exit` / `quit` または EOF（Ctrl+D）で終了する
- REPL は当面ツリーウォーク型インタプリタのみで動作する。MVM バックエンドの
  REPL 対応は将来課題とする。

```
$ myon
myon> x = 1
myon> myon.print(x)
1
myon> exit
```

ファイル実行モード（`myon file.myon`）は従来通り。

---

## 13. EBNF文法定義

```ebnf
program        = { top_level_stmt } ;

top_level_stmt = system_decl | module_decl | statement ;

system_decl    = "system", "myon.useversion", "=", int_literal ;
module_decl    = "module", module_path, [ "as", identifier ] ;
module_path    = identifier, { ".", identifier } ;

statement      = assignment
               | expr_stmt
               | if_stmt
               | while_stmt
               | for_stmt
               | func_decl
               | struct_decl
               | break_stmt
               | continue_stmt
               | return_stmt
               | expose_stmt
               | block ;

statement_list = statement, { ( ";" | newline ), statement } ;

block          = "{", statement_list, "}" ;

assignment     = identifier, [ ":", type ], "=", expression
               | identifier, compound_assign_op, expression ;
compound_assign_op = "+=" | "-=" | "*=" | "/=" ;

expr_stmt      = expression ;

if_stmt        = "myon.if", expression, block,
                 { "myon.elif", expression, "then", block },
                 [ "myon.else", block ] ;

while_stmt     = "myon.while", expression, block ;

for_stmt       = "myon.for", identifier, "myon.in", ( range_expr | expression ), block ;
range_expr     = "range", "(", expression, ",", expression, ")" ;

func_decl      = [ "myon.async" ], "myon.func", identifier, [ type_params ],
                 "(", [ param_list ], ")", "ret", ret_type_list, block ;
type_params    = "<", identifier, { ",", identifier }, ">" ;
param_list     = param, { ",", param } ;
param          = identifier, ":", type ;
ret_type_list  = type, { ",", type } ;

struct_decl    = "myon.struct", identifier, [ type_params ],
                 [ "myon.extends", identifier ],
                 "{", { field_decl | func_decl }, "}" ;
field_decl     = identifier, ":", type ;

break_stmt     = "myon.break" ;
continue_stmt  = "myon.continue" ;
return_stmt    = "ret", expression, { ",", expression } ;
expose_stmt    = "myon.expose", identifier ;

expression     = or_expr ;
or_expr        = and_expr, { "myon.or", and_expr } ;
and_expr       = not_expr, { "myon.and", not_expr } ;
not_expr       = [ "myon.not" ], comparison ;
comparison     = additive, [ comp_op, additive ] ;
comp_op        = "==" | "!=" | "<" | ">" | "<=" | ">=" ;
additive       = multiplicative, { ( "+" | "-" ), multiplicative } ;
multiplicative = unary, { ( "*" | "/" ), unary } ;
unary          = [ "-" ], postfix ;
postfix        = primary, { ".", identifier, [ "(", [ arg_list ], ")" ]
                           | "[", expression, "]"
                           | "(", [ arg_list ], ")" } ;
primary        = literal
               | identifier, [ "<", type, { ",", type }, ">" ]  (* ジェネリック実体化 *)
               | "myon.await", expression
               | lambda_expr
               | "(", expression, ")" ;

lambda_expr    = "myon.lambda", "(", [ param_list ], ")", "ret", type, block ;

arg_list       = arg, { ",", arg } ;
arg            = expression | identifier, "=", expression ;

literal        = int_literal | float_literal | bool_literal
               | str_constructor | char_constructor | error_constructor | string_interp ;

str_constructor   = "str", "(", expression, ")" ;
char_constructor  = "char", "(", expression, ")" ;
error_constructor = "error", "(", expression, ")" ;
string_interp     = '"', { text_fragment | "{", expression, "}" }, '"' ;

type           = "int" | "float" | "str" | "char" | "bool" | "void"
               | "error" | identifier, [ "<", type, { ",", type }, ">" ]  (* struct名／ジェネリック実体化 *)
               | "myon.array", "(", type, ")"
               | "myon.map", "(", type, ",", type, ")" ;

int_literal    = dec_literal | hex_literal | oct_literal ;
dec_literal    = digit, { digit } ;
hex_literal    = "0x", hex_digit, { hex_digit } ;
oct_literal    = "0o", oct_digit, { oct_digit } ;
hex_digit      = digit | "a".."f" | "A".."F" ;
oct_digit      = "0".."7" ;
float_literal  = digit, { digit }, ".", digit, { digit }, [ exponent ] ;
exponent       = ( "e" | "E" ), [ "+" | "-" ], digit, { digit } ;
bool_literal   = "true" | "false" ;

identifier     = letter, { letter | digit | "_" } ;
letter         = "a".."z" | "A".."Z" | "_" ;
digit          = "0".."9" ;
```

---

## 14. 拡張仕様（v0.2確定分）

### 14.1 数値リテラルの拡張

10進数に加えて16進数・8進数・指数表記をサポートする。

```myon
h = 0x1F        // 16進数
o = 0o17         // 8進数
e = 1.5e10       // 指数表記
```

### 14.2 辞書/マップ型

`myon.map(K, V)` を導入する。`myon.array` と対になるコレクション型。

```myon
m = myon.map(str, int)
m.set(str("age"), 20)
v = m.get(str("age"))
m.delete(str("age"))
has = m.has(str("age"))
```

#### マップメソッド（Phase4）

`set` / `get` / `has` / `delete` に加え、中身を列挙するメソッドを備える。

| メソッド | シグネチャ | 説明 |
|------|-----------|------|
| `set` | `(k: K, v: V) ret void` | キー`k`に値`v`を設定 |
| `get` | `(k: K) ret V` | キー`k`の値（無ければ`myon.nil`） |
| `has` | `(k: K) ret bool` | キー`k`が存在するか |
| `delete` | `(k: K) ret bool` | キー`k`を削除（削除できたか） |
| `keys` | `() ret myon.array(K)` | 全キーを要素型`K`の配列で返す |
| `values` | `() ret myon.array(V)` | 全値を要素型`V`の配列で返す |
| `length` | `() ret int` | エントリ数 |

`keys()` / `values()` の要素順序は保証しない（このPhaseでは順序を仕様に
含めない）。順序に依存しない検証をするか、必要なら返った配列を `sort()`
してから使うこと。

```myon
m = myon.map(str, int)
m.set(str("a"), 1)
m.set(str("b"), 2)
m.set(str("c"), 3)
myon.print(m.length())          // 3
ks = m.keys()
ks.sort()
myon.print(ks)                  // [a, b, c]
vs = m.values()
myon.print(vs.contains(2))      // true（順不同なので contains で確認）
```

### 14.3 関数内の制御フロー

早期 `ret` のみをサポートする。goto等の任意ジャンプは導入しない
（明示的で読みやすい構文というMyonの設計思想に合わせるため）。

### 14.4 標準ライブラリの初期ラインナップ

`myon.stdio` に加え、以下を最初から用意する。

- `myon.stdio`：`myon.print`, `myon.input`, および以下のファイルI/O関数
  - `myon.file.read(path: str) ret str, error`
  - `myon.file.write(path: str, content: str) ret bool, error`
  - `myon.file.append(path: str, content: str) ret bool, error`
  - `myon.file.exists(path: str) ret bool`
- `myon.math`：数学関数（10.4節、Phase4で `gcd`/`lcm`/`pow_int` を追加）
- `myon.string`：文字列操作関数（10.4節）
- `myon.time`：現在時刻取得・スリープ（10.5節、Phase4で新設）
- `myon.random`：乱数生成（10.6節、Phase4で新設）

ファイルI/Oの詳細な挙動（エラー伝播など）は10.2節を参照。
その他の具体的な関数シグネチャは今後のAPI仕様書で別途定める。

### 14.5 モジュール循環参照

循環参照は**エラー**とする。モジュール解決時に循環を検出した場合、
コンパイル/実行時エラーとして報告する。

### 14.6 struct継承時のフィールド名衝突

子structが親structと同名のフィールドを持つことは**エラー**とする。
オーバーライドは許可しない（型の厳密性を保つため）。

### 14.7 型推論の範囲

関数の引数・戻り値・struct/フィールドの型注釈は引き続き**必須**。
ただし、リテラル直接代入（右辺が単一リテラルの場合）に限り型注釈を省略できる。

```myon
x = 1          // int と推論（省略可）
y: int = 1     // 明示も引き続き可能

myon.func add(a: int, b: int) ret int { ret a + b }  // 関数は型注釈必須のまま
```

### 14.8 ジェネリクス（型パラメータ）

structおよび関数は型パラメータをサポートする。

```myon
myon.struct Box<T> {
    value: T
}

b = Box<int>(value=5)

myon.func first<T>(xs: myon.array(T)) ret T {
    ret xs[0]
}
```

**型制約は導入しない（確定事項）。** `T: Comparable` のような境界指定は
サポートせず、ジェネリクスは型パラメータへの型の代入のみを行う。任意の型を
渡すことができる。現状のジェネリクス実装が「型の代入のみ」というシンプルな
モデルであることを踏まえ、言語の複雑度を抑える実用性重視の判断としてこの方針を
確定する。将来、型制約がないと困る具体的な利用シーンが現れた段階で改めて検討する。

### 14.9 並行処理・非同期処理（Phase5 で本格化）

`myon.async` / `myon.await` を導入する。

```myon
myon.async myon.func fetchData() ret str {
    myon.await myon.time.sleep_ms(50)
    ret str("結果")
}

result = myon.await fetchData()
```

**実行モデル：シングルスレッド・協調的イベントループ（Phase5 で確定）。**

Phase5 以前の「疑似非同期（呼び出し即同期実行）」は廃止され、次の本格的だが
軽量な非同期モデルに置き換えられた。

- 処理系はプロセス内にただ 1 つの **協調的イベントループ**（`src/event_loop.c`）
  を持つ。
- `myon.async myon.func` を呼び出しても本体はその場では実行されず、
  **タスク（コルーチン）** として生成され、その関数呼び出し式は
  **Task 値**（内部型 `TYPE_TASK`。ユーザーが型注釈に書くことは想定しない）を
  即座に返す。挙動は Python の `asyncio.create_task` に近い。
- `myon.await <Task>` は、対象タスクが完了するまで **現在のコルーチンを協調的に
  一時停止**し、その間イベントループは他の準備完了タスクを実行する。対象が完了
  したら再開し、その戻り値を返す（対象タスク内で実行時エラーが発生していた場合は
  `myon.await` 地点で同じ実行時エラーとして再送出される）。
- I/O 待ち（ソケットの read/write/accept/connect、`myon.time.sleep_ms` 等）は
  OS スレッドをブロックせず、イベントループの `select(2)` 待ちに登録され、
  他のタスクへ制御が回る。
- タスクの切り替わりは **`await` 地点・I/O 待ち地点でのみ** 起こる（協調的
  マルチタスク）。タイムスライスによる強制中断（プリエンプション）は行わない。
  これは Python asyncio / JavaScript async-await と同じモデルである。
- `myon.async` を使わない同期的なコード（トップレベルスクリプトなど）から
  `myon.time.sleep_ms` や `myon.net.*` を呼んだ場合は、従来どおりの
  **ブロッキング動作にフォールバック**する（後方互換）。

**後方互換性**：オペランドが Task 値でない（= `myon.async` でない普通の式に
`myon.await` を書いた）場合、`myon.await` はその値をそのまま返す。これにより
Phase5 以前の擬似非同期を前提としたコード（`result = myon.await fetchData()` の
ような単一値返却）はそのまま動作する。

**コルーチンの実装方式**：POSIX `ucontext.h` の `makecontext`/`swapcontext` を用い、
各タスクに専用の C スタック（256KB）を割り当てる方式（方式A）を採用した。これに
より既存のツリーウォークインタプリタ（`eval_expr`/`exec_stmt` の再帰呼び出し）を
一切変更せずに済む。各タスクは自分専用の C スタック上で通常どおり再帰し、`await`／
I/O 待ち地点で `swapcontext` によりループ本体へ制御を返す。

**採用しなかった選択肢**：本物の OS スレッド化（マルチコア並列実行）は採用しない。
値の参照カウント（`refcount`）をスレッドセーフにする大改修が必要で、処理系全体への
侵襲が大きすぎるため（Phase2 P6 の判断を継続）。プリエンプティブなタスク切り替えも
行わない。

**対応プラットフォーム**：イベントループは Linux（glibc）と Windows の双方で
本実装済みである。`ucontext`／Fiber のいずれも欠くプラットフォームでは未対応
スタブ（`event_loop_supported()` が `0` を返す）としてコンパイルされる
（FFI サブシステムと同じポリシー）。

**Windows 実装（Win32 Fiber、Phase5 Step3）**：`ucontext` は Windows に存在
しないため、Windows では同等のユーザーモード・スタック切り替え機構である
**Win32 Fiber API** でコルーチンを実装する（`src/event_loop.c` の
`#elif defined(_WIN32)` 分岐、マクロ `MYON_EVENT_LOOP_FIBER`）。Linux 版
（`MYON_EVENT_LOOP_UCONTEXT` 分岐）は一切変更していない。API 対応は次の通り
（Microsoft Learn の winbase.h／winsock2.h リファレンスで確認）：

| ucontext（Linux） | Fiber（Windows） | 役割 |
|---|---|---|
| `getcontext`＋`makecontext` | `CreateFiber(stack, proc, param)` | タスク用スタック確保＋エントリ設定 |
| `swapcontext` | `SwitchToFiber(fiber)` | コンテキスト切り替え |
| `loop->core_ctx` | `ConvertThreadToFiber(NULL)`（`main_fiber`） | ループ本体（復帰先） |
| （`uc_stack` の `malloc`） | `CreateFiber` の第1引数（256KB） | スタックは Fiber API が内部確保 |
| `uc_link` による自動復帰 | **なし**（下記の注意点を参照） | タスク終了時の復帰 |
| `free(stack)` | `DeleteFiber(fiber)` | スタック破棄 |

`CreateFiber` はエントリ関数へパラメータを直接渡せるため、`ucontext` 版に
あったグローバル変数 `g_bootstrapping_task` によるバイパスは不要になり、
`Task*` を素直に渡している。時刻取得は `clock_gettime(CLOCK_MONOTONIC)` の
代わりに `GetTickCount64()`（システム起動からの単調増加ミリ秒）を用いる。
I/O 多重化は Winsock の `select()` を用いる（`fd_set` は `SOCKET` の配列で、
第1引数 `nfds` は無視されるため `0` を渡す。fd → `SOCKET` の復元は 10.7 を
参照）。

**Fiber のライフサイクル上の最重要注意点（`uc_link` 相当の不在）**：Fiber の
エントリ関数（fiber proc）は**決して `return` してはならない**。return すると
`ExitThread` が呼ばれスレッドごと終了する（`ucontext` の `uc_link` のような
「呼び出し元へ自動復帰する」仕組みが Fiber には無い）。そこで Windows 版の
`task_fiber_proc` は、タスク本体 `entry()` の完了後に状態を `TASK_DONE` にした
うえで**無限ループに入り、`SwitchToFiber(main_fiber)` でループ本体へ制御を
返し続ける**設計とした。スケジューラは `DONE` のタスクを二度と `SwitchToFiber`
しないため、このループ本体は完了時に一度だけ実行され、以後 Fiber は
`DeleteFiber` されるまで待機（park）状態で留まる。`DeleteFiber` は
`event_loop_destroy()`（ループ本体＝別 Fiber）から、待機中（＝実行中でない）の
タスク Fiber に対して呼ぶ。Microsoft のドキュメント上、実行中の Fiber を
他 Fiber から削除するのは危険だが、破棄時点で全タスク Fiber は
`SwitchToFiber` 地点で待機しており実行中ではないため安全である。

---

## 15. Open Questions（残存する未決定事項）

- `myon.map` のキー型に許される範囲（str/int以外の任意型を許すか）
- 型推論をリテラル以外の式（関数呼び出し結果等）にも広げるか

Phase5（`myon.net`/`myon.http`）で今回スコープ外とし、今後の検討課題として
残した事項：

- **IPv6対応**：現状の `myon.net` はIPv4のみ。`AF_INET6` の追加をどう表現するか
- **TLS/HTTPS対応**：`myon.http.get`/`post` の `https://`、およびTLSサーバー。
  自前実装は非現実的なため外部ライブラリ（OpenSSL等）への依存をどう扱うか
- **HTTPサーバーのKeep-Alive対応**：現状はHTTP/1.0・1コネクション1リクエスト
  固定。`Connection: keep-alive` / HTTP/1.1 chunked をサポートするか
- **レスポンスヘッダ・ステータスコードのカスタマイズ**：`serve` のハンドラは
  現状ボディ文字列のみを返し、常に `200 OK` / `text/plain` 固定。任意の
  ステータス・ヘッダを返せるAPI形状（構造体を返す等）をどうするか
- **本物のマルチスレッド化**：現状の並行処理は単一スレッドの協調的コルーチン
  （14.9節）。OSスレッド/マルチコア並列をどう表現するか（当面は導入しない）

> 以下は Phase 5 で確定済みのため本リストから除外した。
> - `myon.async`/`myon.await` の実行モデル → 単一スレッドの協調的イベント
>   ループ（ucontextベースのコルーチン）に確定（14.9節）
> - ジェネリクスの型制約 → 導入しないことに確定（14.8節）
>
> 以下は Phase 3.5 で確定済みのため本リストから除外した。
> - 標準ライブラリ `myon.math`/`myon.string` の具体的な関数シグネチャ一覧
>   → 全関数のシグネチャを 10.4節 に確定

---

## 16. 総合サンプルコード

```myon
system myon.useversion=1
module myon.stdio

myon.struct Person {
    name: str
    age: int

    myon.func greet() ret void {
        myon.print("Hello, my name is {self.name}")
    }
}

myon.func divide(a: int, b: int) ret int, error {
    myon.if b == 0 then {
        ret 0, error("ゼロ除算")
    }
    ret a / b, myon.nil
}

x = str("人間")
a = char("a")
b = 1

myon.print("Hello Worlddd! ", x + "!")

myon.if a > b {
    myon.print("aがbよりデカい")
} myon.elif b > a then {
    myon.print("bがaよりデカい")
} myon.else {
    myon.print("しらね")
}

p = Person(name=str("太郎"), age=20)
p.greet()

result, err = divide(10, 0)
myon.if err != myon.nil then {
    myon.print("エラー: {err}")
} myon.else {
    myon.print("結果: {result}")
}

xs = myon.array(int)
xs.push(1)
xs.push(2)
myon.for item myon.in xs {
    myon.print("{item}")
}
```
