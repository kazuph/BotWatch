# FACES ARCADE

M5Stack Core2 と Faces Gamepad3 で動く、学習とゲームのアプリです。

- 音あて、和音あて、かな読み（10問連続正解で終了）
- 周波数メーター（Faces のボトムにはマイクが無いため「マイクなし」と表示）
- ブロック、へび、ぱたぱた
- SELECT でメニューへ戻る

## 準備

`platformio.ini` は `../M5Faces` を参照します。このフォルダの隣に [m5stack/M5Faces](https://github.com/m5stack/M5Faces) を置いてください。

かな読みの音声 `kana_voice.bin` は git に入れていません。sanoTTS-jp のモデルライセンス（`../stopwatch-voice/licenses/sanotts/LICENSE-MODEL.md`）により、生成した音声を素材として配布しないためです。次の手順で手元に作ります。モデルの取得は `../stopwatch-voice/README.md` の手順に従います。

```sh
cc -O2 -I tools/sanotts/src tools/gen_kana.c tools/sanotts/src/*.c -lm -o tools/gen_kana
tools/gen_kana <モデルのbin> kana_voice.bin src/kana_voice.h あ い う …
```

かなの一覧と順番は `src/kana_voice.h` のとおりです。ぢ・づは じ・ず と同じ音なので入れていません。

この手順で作り直すと、一部の長さが書き込み済みの版と数百サンプル変わることがあります（2026-11-02 に確認）。作り直すときは `src/kana_voice.h` も一緒に上書きしてください。

## ビルド

```sh
uvx --with intelhex platformio run
```
