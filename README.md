# am312_config

AM312 人感センサー + XIAO BLE で、人が近づいたらキー操作を送る ZMK ファームウェア。

## 送るキーを変える

1. [docs/index.html](docs/index.html) をブラウザで開く
2. 送るキーの手順を決めて「設定ファイルをダウンロード」を押す
3. XIAO BLE のリセットボタンを2回押し、出てきたドライブに `pir_config.uf2` をコピーする

設定ファイルはフラッシュの設定領域（`0xE9000`、4KB）だけを書き換えるので、ファームウェア本体を書き直す必要はない。
ファームウェアを更新しても設定は残る。初期設定に戻すときは「初期設定に戻すファイル」を書き込む。

## 構成

| パス | 内容 |
|---|---|
| `config/boards/shields/pir_switch/` | シールド定義（配線・キーマップ・設定領域） |
| `src/behavior_pir_config_macro.c` | 設定領域を読んでキーを送る ZMK の動作 |
| `dts/bindings/`, `Kconfig`, `CMakeLists.txt`, `zephyr/module.yml` | 上の動作を ZMK に組み込むための定義 |
| `docs/index.html` | 設定ファイル（.uf2）を作る Web ページ |
