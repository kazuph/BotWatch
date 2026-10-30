# BotWatch

BotWatch は M5Stack StopWatch で音声認識・キャラクター表示・TTS・ラベル印刷を行うプロジェクトです。
画面の長押しで開くアプリ一覧から、チューナー・スペクトル・耳のゲーム・ストップウォッチ・ポモドーロ・時計・押して話す無線も使えます。
BLEラベルプリンター「2523/GHLP_BLE」（Deli系）をPC・Raspberry Piから利用する道具も含みます。
本体のビルド・ペット・個人用プリセットは [stopwatch-voice/README.md](stopwatch-voice/README.md) を参照してください。
プロトコルは [docs/PROTOCOL.md](docs/PROTOCOL.md)。

## Mac / Raspberry Pi（Python + bleak）

[uv](https://docs.astral.sh/uv/) があれば依存は自動で入る。プリンターは同時に1台としかつながらないので、スマホのアプリは閉じておく。

```sh
uv run tools/scan.py                          # 近くの BLE 機器を一覧
uv run tools/query.py                         # 状態・電池・ファーム版などを照会（紙を使わない）
uv run tools/print_text.py "さとうじろう"    # 文字ラベルを印刷
uv run tools/print_text.py "枠つき" --border --preview out.png   # 枠つき、プレビュー画像も保存
uv run tools/print_text.py "確認だけ" --dry-run --preview out.png  # 送らずにプレビューだけ
uv run tools/calibrate.py                     # 印字幅を調べる階段パターン
uv run tools/ruler.py                         # 長さ方向の定規
```

ライブラリとして使う場合:

```python
from labelprinter.ble import Printer
from labelprinter import protocol
from labelprinter.render import text_label

p = await Printer.find("GHLP")
await p.write(protocol.print_job(text_label("こんにちは")))   # 任意の PIL 画像も可（幅 96 ドット）
```

Raspberry Pi では日本語フォント（`fonts-noto-cjk`）を入れておく。

## M5Stack StopWatch（ESP32-S3）

`m5stack/` を PlatformIO でビルドする（`uvx platformio run -d m5stack`、書き込みは `-t upload`）。
起動すると自動でプリンターに接続し、ボタン A か画面タップで `LABEL_TEXT` を印刷する（実機で印刷確認済み）。
