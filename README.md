# am312_config

AM312 人感センサー + XIAO BLE で、人が近づいたらキー操作を送る ZMK ファームウェア。

## 配線

| 部品 | XIAO BLE |
|---|---|
| AM312 VCC / GND / OUT | 3V3 / GND / D0 |
| スイッチA（モード1・緑） | D1 と GND の間 |
| スイッチB（モード2・青） | D2 と GND の間 |
| 緑LED | D3 → 抵抗 → LED(+)、LED(-) → GND |
| 青LED | D4 → 抵抗 → LED(+)、LED(-) → GND |

D4 は標準では I2C 用だが、LED に使うため I2C は無効にしている。

## 本体の操作

| 操作 | 動作 |
|---|---|
| センサー反応 | 今のモードのキーを送る。次に反応できるまでモードの色のLEDが点く |
| スイッチA / B 単押し | 動作確認（今のモードのキーを送り、モードの色で点滅） |
| スイッチA 3秒長押し | モード1（緑）にする |
| スイッチB 3秒長押し | モード2（青）にする |
| 両方 単押し | センサー反応時のLED表示を ON / OFF |
| 両方 3秒長押し | Bluetooth のペアリング情報をすべて消す |

モードと LED の ON/OFF は電源を切っても保たれる。

## 送るキーを変える

1. [docs/index.html](docs/index.html) をブラウザで開く
2. モードごとに送るキーの手順を決めて「設定ファイルをダウンロード」を押す
3. XIAO BLE のリセットボタンを2回押し、出てきたドライブに `pir_config.uf2` をコピーする

設定ファイルはフラッシュの設定領域（`0xE9000`、4KB）だけを書き換えるので、ファームウェア本体を書き直す必要はない。
ファームウェアを更新しても設定は残る。初期設定に戻すときは「初期設定に戻すファイル」を書き込む。

## 構成

| パス | 内容 |
|---|---|
| `config/boards/shields/pir_switch/` | シールド定義（配線・キーマップ・設定領域） |
| `src/behavior_pir_switch.c` | センサー・スイッチ・LED・設定領域を扱う ZMK の動作 |
| `include/dt-bindings/zmk/pir_switch.h` | キーマップで使うコマンド（`PIR_TRIGGER` など） |
| `dts/bindings/`, `Kconfig`, `CMakeLists.txt`, `zephyr/module.yml` | 上の動作を ZMK に組み込むための定義 |
| `docs/index.html` | 設定ファイル（.uf2）を作る Web ページ |
