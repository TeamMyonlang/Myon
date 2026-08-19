# Known Issues (既知の不具合)

このファイルは、セキュリティ監査（`src/` のマニュアルレビュー）の過程で
見つかった**セキュリティ以外のバグ・改善点**を記録したものです。
セキュリティ上の脆弱性は同じレビューでコード修正済み（下記「参考」を参照）で、
ここに列挙する項目は今回は**修正せず**記録のみとしています。

> レビュー日: 2026-08-13
> 対象コミット時点の `main` ブランチ / `src/` 全体

---

## 参考: 今回コードで修正したセキュリティ項目（記録のみ）

- **HTTP クライアント応答の非有界バッファ（クライアント側 DoS）** —
  `http_client_request()`（`src/interpreter.c`）が応答を `cap *= 2` で
  上限なく読み続けていた。悪意あるサーバ（TLS 検証はベストエフォートのため
  MITM を含む）が無限に応答を流すと `myon_xrealloc` が伸び続け、最終的に
  `myon_xmalloc` の OOM 経路でインタプリタ全体が異常終了（exit 70）していた。
  → 応答バッファを **64 MiB** で打ち切り、明示エラーを返すよう修正。
- **`parse_content_length()` の入力検証不足（多層防御）** —
  `strtol()` の戻り値を未検証で返していた。負値・範囲外（`ERANGE`）を 0 に
  丸めるよう修正。

---

## 非セキュリティのバグ・改善点（未修正・記録のみ）

### 1. `longjmp` によるスタック巻き戻しで解放されないメモリ ✅ 修正済み

- **場所**: `src/interpreter.c`（`runtime_error()` → トップレベル `setjmp`）
- **内容**: 実行時エラーは `longjmp` でトップレベルまで一気に巻き戻るため、
  途中フレームが確保した `call_env` や一時 `Value` が解放されずに残る
  （valgrind 上は `definitely/indirectly lost`）。
- **影響**: プロセス終了時に OS が回収するため実害はないが、
  `*.err` テストや深い再帰のリミット到達時にリークとして計上される。
- **修正**: 所有権モデル全体には手を入れず、**登録ベースのクリーンアップ
  スタック**を局所的に導入した（`Interp.cleanup_stack`）。
  - `call_function()` は各呼び出しの `call_env` を入口で `cleanup_push()`
    し、正常終了（return/void）で `cleanup_pop()` してから `env_free()` する。
  - 各 `setjmp` バリア（`interpret` / `interp_run` / `async_task_entry` /
    `http_conn_task_entry` / `myon_ffi_callback_dispatch`）は入場時の深さを
    `cleanup_mark()` で記録し、`longjmp` を捕捉したら `cleanup_unwind()` で
    その深さまで巻き戻し、飛び越された各 `call_env` を `env_free()` する。
  - 引数型不一致の早期エラー経路は、二重解放を避けるため `env_free()` の直前に
    `cleanup_pop()` してからエラーを送出する。
  - `interp_free()` / `interpret()` の終了処理では、正常終了なら空・エラー時は
    巻き戻し済みのため、残った backing 配列を `free()` するだけでよい。
  - これにより `*.err` テストや再帰リミット到達時の途中フレームのリークが
    解消される。所有権モデルの大規模リファクタやアリーナアロケータ化は不要。

### 2. HTTP レスポンスのステータス行が常に 200 固定（`myon.http.serve`） ✅ 修正済み

- **場所**: `src/interpreter.c` `http_conn_task_entry()`
- **内容**: ユーザハンドラ経由のレスポンスは常に
  `http_build_response(200, "OK", "text/plain", ...)` で構築される。
  ハンドラが 404/500 等を返す手段がなく、`Content-Type` も固定。
- **影響**: 機能上の制限（正しくないステータス/型を返せない）。
  セキュリティではないが、実用 HTTP サーバとしては不便。
- **修正**: ハンドラの戻り値でステータス/`Content-Type` を指定できるように
  した（後方互換）。
  - `str` を返す → 従来どおり `200 OK` / `text/plain`
  - 配列を返す → 末尾要素が本文、先頭がステータス（`int` または `"404"`
    のような数値文字列）、3 要素なら 2 番目が `Content-Type`
    - `[body]` → `200` / `text/plain`
    - `[status, body]` → `status` / `text/plain`
    - `[status, content_type, body]` → `status` / `content_type`
  - 理由句は `http_status_text()`（`src/http.c` に追加）で解決。ハンドラ内で
    実行時エラーが起きた場合は `500` を返す。

### 3. 静的ファイル配信の `ftell`/`fread` サイズ不一致を無視 ✅ 修正済み

- **場所**: `src/interpreter.c` `http_serve_static_conn()`
- **内容**: `ftell` で得たサイズ `fsz` で確保し `fread` するが、`fread` の
  実読込量 `rd` と `fsz` の差（ファイルが読込中に短くなる等）を検査しない。
  レスポンスには `rd` を使っているためオーバーリードは無いが、
  ディレクトリを `fopen("rb")` できてしまう環境では `ftell` が不定になり得る。
- **影響**: 実害は限定的（`rd` 基準で送出するため境界は安全）。
  ただしディレクトリ/特殊ファイルに対する挙動が未定義寄り。
- **修正**: Linux では `fopen` 前に `stat()`＋`S_ISREG()` で通常ファイルか
  判定し、ディレクトリ/FIFO/デバイス等は `404` を返すようにした
  （`sys/stat.h` を追加）。非 Linux は従来動作を維持。

### 4. `http_parse_url()` の IPv6 リテラル未対応 ✅ 修正済み

- **場所**: `src/interpreter.c` `http_parse_url()`
- **内容**: `host[:port]` を素朴に `:` で分割するため、`[::1]:8080` の
  ような IPv6 リテラル URL を正しく解釈できない（最初の `:` で切れる）。
- **影響**: 機能上の制限。IPv6 のホストへ HTTP クライアントで接続できない。
- **修正**: 先頭が `[` のときは `]` までをホストとして抽出し、`]` の直後の
  `:` のみをポート区切りとして解釈するようにした。括弧が閉じない場合は
  明示エラーを返す。

### 5. `net_raw_fd()` の Windows における `SOCKET`→`int` 切り詰め ✅ 修正済み

- **場所**: `src/net.c`（`_WIN32` 分岐）
- **内容**: Windows の `SOCKET`（`UINT_PTR`, 64bit）を `int` へ切り詰めている。
  Winsock のカーネルハンドルは 32bit に収まると文書化されているため
  「実運用上は安全」だが、型としては不正確でコメントにも hand-off 注記が残る。
- **影響**: Linux では無関係（`SOCKET` == `int` 相当で切り詰めが起きない）。
  Windows 実機は未検証（README 記載）。
- **修正**: 恒久対策として、raw fd/socket を運ぶ型を `intptr_t` 相当の
  新しい型 **`myon_fd_t`**（`src/net.h`、無効値は `MYON_INVALID_FD`）に一斉に
  広げた。
  - `net_raw_fd()` の戻り値、`net_sync_wait_fd()` の引数（`net.c`）、
    `event_loop_wait_readable/writable()` と `Task.waiting_fd`
    （`event_loop.h`/`event_loop.c`）、および `interpreter.c` 内で fd を
    持ち回るローカル変数（`net_wait_fd()` の引数含む）をすべて `myon_fd_t`
    に統一した。
  - Windows 分岐では `(int)`／`(unsigned int)` によるゼロ拡張の再構成トリックが
    不要になり、`SOCKET` の完全な幅がそのまま往復する
    （`fd_to_socket()` は `(SOCKET)(UINT_PTR)fd` のみ）。
  - Linux では `myon_fd_t` は実質 `int` の値域であり、`FD_SET`/`select()` に
    渡す直前に `(int)` へ戻す（切り詰めは起きない）。挙動は従来どおり。
- **検証（CI）**: `src/net.c` の `_WIN32` 分岐に
  `static_assert(sizeof(myon_fd_t) >= sizeof(SOCKET), ...)` を追加し、
  型が再び狭くなった場合に **Windows ビルド（`win-cross` / `win-native`）を
  コンパイル時に失敗させる**ようにした。さらに `.github/workflows/
  heavy-checks.yml` の `win-cross` ジョブに検証ステップを追加し、
  (1) `net.c` が Windows 向けにコンパイルできること（正）と、
  (2) `myon_fd_t` を `int` に狭めた対照テスト用 TU が **この static_assert で
  失敗すること**（負の対照）を確認する。これにより「ガードが空虚に真ではない」
  ことを CI 上で能動的に示す。

### 6. `myon.random` は暗号学的に安全でない ✅ 対応済み（別 API を追加）

- **場所**: `src/interpreter.c`（`myon.random`、`srand`/`rand` ラップ）
- **内容**: 乱数は `rand()` ベースで、暗号用途には不適。
- **影響**: 仕様どおり（README にも明記）。トークン/鍵生成に誤用すると危険。
- **修正**: 暗号用途向けの新 API `myon.random.secure_int(lo, hi)` を追加した。
  OS の CSPRNG（Linux: `/dev/urandom`）から乱数を取得し、剰余バイアスを
  除くために棄却サンプリングを行う（区間 `[lo, hi]` は一様）。CSPRNG が
  利用できない環境ではエラーを返す（弱い値にフォールバックしない）。
  既存の `myon.random.int`/`float` は互換のため `rand()` ベースのまま
  （非暗号用途向け）。

---

## メモ

- 上記のうち **1・5・6** はもともと README に既述の既知の性質・制限でした
  （重複記録ですが、レビューの網羅性のため再掲）。**1・5** は本変更で修正済み。
- **2・3・4** は今回のレビューで新たに整理した非セキュリティの改善点です。
- 対応状況:
  - **✅ 修正済み**: **1**（`longjmp` リーク＝登録ベースのクリーンアップ
    スタックを局所導入）、**2**（HTTP ステータス/ヘッダ指定）、**3**（通常
    ファイル判定）、**4**（IPv6 リテラル）、**5**（`net_raw_fd` の型を
    `myon_fd_t`＝`intptr_t` 相当へ拡張し、Windows CI に静的アサート検証を追加）、
    **6**（暗号用途向け `secure_int` を追加）。
  - **未対応**: なし（本ファイルに記録した非セキュリティ項目はすべて対応済み）。
