# FACES ARCADE

M5Stack CoreS3 Lite または Core2 と Faces Gamepad3 で使う、学習とゲームのアプリです。音あて・和音あて・かな読み・周波数メーター・ブロック・へび・ぱたぱたがあり、SELECT でメニューに戻ります。CoreS3 ではカメラも使います。

## 準備

ビルドには Docker と Espressif の `espressif/idf:v5.5.5` イメージを使います。依存ライブラリは ESP-IDF Component Manager が取得します。

かな読みの音声 `kana_voice.bin` は git に入れていません。sanoTTS-jp のモデルライセンス（`../stopwatch-voice/licenses/sanotts/LICENSE-MODEL.md`）により、生成した音声を素材として配布しないためです。次の手順で手元に作ります。モデルの取得は `../stopwatch-voice/README.md` の手順に従います。

```sh
cc -O2 -I tools/sanotts/src tools/gen_kana.c tools/sanotts/src/*.c -lm -o tools/gen_kana
tools/gen_kana <モデルのbin> kana_voice.bin src/kana_voice.h あ い う …
```

かなの一覧と順番は `src/kana_voice.h` のとおりです。ぢ・づは じ・ず と同じ音です。作り直すと一部の長さが既存版と数百サンプル変わるため、`src/kana_voice.h` も一緒に更新してください。

## ビルド

`faces-arcade` ディレクトリで実行します。機種ごとに設定とビルド先を分け、CoreS3 の完了後に Core2 をビルドします。部品の取得結果は `.component_cache` に保存し、Git の取得には HTTP/1.1 を使います。

```sh
docker run --rm -v "$PWD:/project" -w /project \
  -e IDF_COMPONENT_CACHE_PATH=/project/.component_cache \
  -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=http.version -e GIT_CONFIG_VALUE_0=HTTP/1.1 \
  espressif/idf:v5.5.5 \
  idf.py -B build-cores3 -D SDKCONFIG=sdkconfig.cores3 -D IDF_TARGET=esp32s3 build

docker run --rm -v "$PWD:/project" -w /project \
  -e IDF_COMPONENT_CACHE_PATH=/project/.component_cache \
  -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=http.version -e GIT_CONFIG_VALUE_0=HTTP/1.1 \
  espressif/idf:v5.5.5 \
  idf.py -B build-core2 -D SDKCONFIG=sdkconfig.core2 -D IDF_TARGET=esp32 build
```

書き込みにはビルド終了時に表示される `esptool` コマンドを使い、接続先のシリアルポートを指定します。スコアは NVS の `arcade` に保存します。Core2 の従来版と同じ NVS の位置・サイズ・キー・整数型を使います。

## カメラと SD

CoreS3 のカメラでは A で撮影し、FAT32 の microSD に `/DCIM/FACES/IMG_####.JPG` として保存します。番号は既存の最大番号の次です。B で顔検出・物の分類・文字・説明を切り替え、START で最後の写真を表示します。文字と説明の結果は上下でスクロールします。SD がないときや保存できないときは画面に理由を表示します。

Faces Bottom3 の SELECTION スイッチは Core2/CoreS3 側に合わせます。底面の LED は CoreS3 で使い、各色の明るさをゆっくり変えます。アプリを出るとゆっくり消灯します。

## シリアル自己テスト

通信速度は 460800 bps、各命令の末尾に改行を付けます。`p<16進数>` はボタンの押下状態、`s` は画面の RGB データ、`f<Hz>` は周波数の計算確認用の正弦波（0 で解除）、`l<0|1>` は Core2 マイクのチャンネル、`r` はスコアの消去、`t<x>,<y>` はタッチ、`m<0|1>` は消音設定です。`j` は SD の最新 JPEG をシリアルへ送ります。

`r` はスコアだけを消します。通信情報とは別の保存領域です。シリアルから受けた Wi-Fi 情報と認証キーは画面やログに表示しません。

### Wi-Fi と認証キー

対話端末から次の道具を起動し、Wi-Fi の SSID、パスワード、Tailscale の認証キーを入力します。入力は表示されず、値をコマンド引数に渡しません。

```sh
uv run --with pyserial tools/provision.py store --port /dev/cu.usbmodem11401
```

通信設定だけを消すときは、同じ道具の `clear` 操作を使います。スコアは消えません。

```sh
uv run --with pyserial tools/provision.py clear --port /dev/cu.usbmodem11401
```

文字認識は `100.80.152.112:8016`、写真の説明は既定で `100.80.152.112:8193` の `vision` を使います。説明先は ESP-IDF の設定で変更できます。画像は指定された 3090 のアドレスにのみ送ります。

MicroLink は本家 `216da3300f0493b0860247d43f7af5ce29df63a5` に固定します。接続の再試行と送信時の無期限待ちを避けるため、`patches/microlink-tcp-deadline.patch` を構成時に適用します。取得元の版と適用後の差分が一致しないときはビルドを止めます。
