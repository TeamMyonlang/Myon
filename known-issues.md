# Known Issues (既知の不具合)

## パッケージ管理: MVM／`.myc` での package module 取り込み未対応（仕様 §6.3）

インストール済みパッケージの module 取り込み（`module <package-module> as ...`）は、
現状ツリーウォーク型インタプリタ（`.myon` 実行）でのみ対応しています。
`--compile`／`--run-mvm`／`.myc` 実行では、MVM に外部／package module linking の
仕組みがまだ無いため、曖昧にフォールバックせず**明示的なエラー**で失敗します。

該当プログラムはツリーウォーク実行（`.myon`）で動かしてください。MVM 側の
module linking は別の設計課題として今後対応予定です。
