# BotWatch — 音声で動くキャラクターとラベル印刷

BotWatch は M5Stack StopWatch 上で音声認識・TTS・キャラクター表示・家電パネル・BLEラベル印刷を行います。
ひらがな認識と常時ヒアリングを切り替え、1体表示・左右スワイプ・集合・タップ選択に対応します。
ペットを置かない場合は3社キャラクターだけで動作します。

## 取得とビルド

```sh
python3 scripts/fetch_model.py
python3 scripts/fetch_koeact.py   # うごきモード用（KoeAct-JP v0.1.1）
python3 scripts/fetch_tts.py
uv run --with pillow python scripts/build_sprites.py --report /tmp/botwatch-pets.json
docker run --rm -v "$PWD:/project" -w /project espressif/idf:v5.5.5 idf.py build
```

依存はESP-DL 3.3.13、ESP-DSP 1.8.2、M5Unified 0.2.27、M5GFX 0.2.34。
`dependencies.lock` はこの組で解決したものを記録します。
モデルは4,334,768バイト、SHA256
`80db75b2ed89e74948eeac0259919ea5a49cd684d5f14c7c94d5aee5a0c4323c`。
取得スクリプトは実体とハッシュを照合し、不一致は失敗にします。
モデル・ビルド・managed_componentsはgit対象外です。

## 書込みと復元

ビルドが生成した `build/flash_args` を正とし、Macで次を実行します。
パスはbuildを基準としているので、その中で実行します。
USBポートは接続のたびに `/dev/cu.usbmodem*` を確認して指定します。
モデルを含めて現プログラムを上書きします。

```sh
(cd build && uvx esptool --chip esp32s3 --port /dev/cu.usbmodem1101 write-flash @flash_args)
```

区画は現在factoryアプリ `0x10000`（領域長 `0x2F0000`）、モデル
`0x300000`（実体長 `4,334,768`）、うごきモード用の `koeact` `0x740000`
（4,813,760バイト）と `a2` `0xBE0000`（1,132,544バイト）、16MB flashです。
以下の経緯の段落はうごきモード追加前の値です。ブートローダーと
区画表のアドレス・各ファイルの実サイズは生成したflash_argsと成果物で確認します。
初版はアプリ1,917,040バイト、モデル0x200000でした。Noto追加後の実測
2,393,488バイトが旧領域を超えたため、`scripts/fit_partition.py` で
実サイズを64KiB境界へ切り上げ、モデル開始位置を変更しています。
ブートローダー0x0から22,288バイト、区画表0x8000から3,072バイト、
モデル0x260000から4,334,768バイトを送ります。現在のアプリの正確な長さは
検査が保存するJSONを参照してください。
`uv run --with pyyaml scripts/verify_build.py --output <証拠JSONの絶対パス>` で
生成物の設定・依存・ハッシュ・区画内の収まり・書込範囲の重なりを検査します。
BLEとフォント追加後に最終サイズを改めて測定します。

元の印刷サンプルへ戻すときはリポジトリのルートで実行します。

```sh
uvx platformio run -d m5stack -t upload
```

## 実機検証

マイク開始と録音キューの戻り値を確認し、起動直後の1秒を捨ててから待機画面を出します。
その区間が全部ゼロなら準備失敗を表示します。マイクは再録音まで起動を維持します。
既定16倍では実録音が飽和し、1倍だけでは直近2件の認識が欠けました。
保存6件の比較から、1倍で4秒録音し、先頭の完全なゼロを除いて末尾に同数のゼロを補い、
100フレーム×10msごとにピーク基準で最大16倍まで増幅します。上限16はM5Unified既定値、
ピークの上下限はMic_Class.inlのsigned int16飽和値から導出しています。
100フレームごとに確認するため、冒頭の衝撃音だけで後続の声の増幅量が下がりません。
`RAW_PCM` は加工前、`PCM` は認識に使用した加工後の4秒です。音割れは加工前の
`MIC_RAW clipped` と保存したraw-microphone.pcmで判定します。加工後の端値到達とは区別します。
この処理は録音中に1秒分が届くごとに行い、完成した特徴量からstream100モデルへ流します。
録音データは500msごとに受け取り、正規化区間は1秒のままです。先頭の完全ゼロが1サンプルでも
あると、1秒ごとの受渡しでは完成区間が次の秒まで遅れるため、要求された500msの窓内で受け渡します。
先頭の完全ゼロ除去は全体で1回だけ行い、1秒区間の途中のゼロや非ゼロ音声は削りません。
加工前の4秒は128KBの明示的なPSRAMバッファに保持し、確保失敗なら停止します。
`MIC tail_us` は最後のサンプル到着から結果が画面へ送信されるまでを含みます。
大きいPCMのシリアル出力は画面表示後に行い、結果の表示を遅らせません。
待機後のタップ、またはシリアル `MIC` で4秒録音し、`PCM`、`TEXT`、`MEM` を出します。
`INFO` は保存した初期化・モデル・メモリ報告を再表示します。BOOTのboot_idは起動ごとに
変わり、reset_reasonを併記します。保存情報の再表示を新しい起動と混同しません。
タッチとボタンA/Bの実イベントもシリアルへ出します。

```sh
uv run --with-requirements reference/requirements.txt scripts/gain_study.py \
  --old-session /absolute/path/to/four-old-recordings \
  --unity-session /absolute/path/to/two-unity-recordings --output /absolute/path/to/study
python3 scripts/check_audio_gain.py --study /absolute/path/to/study/comparison.json
uv run --with pyserial --with esptool==5.6.0 scripts/monitor.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/recording --reset
```

`--reset` は接続後にポートを閉じ、`python -m esptool --chip esp32s3 --port PORT
--after hard-reset chip-id` でUSBブートローダへ入ってから再起動し、再接続して記録を継続します。
実行前後のboot_idが変わったことを確認してから起動直後の発話を依頼します。

保存した加工前PCMは`RLMS`+64000個のlittle-endian int16で、同じ正規化・特徴量・推論処理へ
500msごとの到着として渡せます（正規化は1秒単位）。これはシリアル診断であり、音を再生せずマイク成功の代替にもしません。
元の`RLMA`は加工後PCMを直接認識する仕様のままです。

```sh
uv run --with pyserial scripts/check_stream.py --session /absolute/path/to/recorded-session \
  --output /absolute/path/to/stream-proof --port /dev/cu.usbmodem1101 --seconds 120
```

元の正規化PCM・99ステップ・文字との完全一致と、画面表示まで500ms以内を検査します。

```sh
uv run --with pyserial scripts/capture.py --port /dev/cu.usbmodem101 \
  --output /absolute/path/to/evidence/cold --seconds 120
```

これは人間のタップを待ち、取得したPCMを同じ本体へRLMAで送り直します。
4秒64,000サンプル・完全な受信・トークンと文字の一致を検査し、PCM/WAV・特徴量・
生ログ・比較JSONを保存します。`--mic` はホストから録音を開始し、
`--info-only` は録音せず初期化報告だけを取得します。`--seconds` はホストの観察期限です。
生成WAVを自動再生しません。

冷起動と再録音の両方で、人間が実際に話した文・タッチ・ボタン・画面の確認を別途記録します。
全ゼロの先頭サンプル数、クリップ数、秒別RMSはJSONへ出しますが、これだけで
発話の先頭が保存されたとは判定しません。MacのFP32比較も段1の必須証拠です。公開の `inference.py` を変更せず使用します。

```sh
python3 scripts/fetch_fp32.py
uv run --with-requirements reference/requirements.txt python reference/inference.py \
  /absolute/path/to/evidence/cold/microphone.wav --model-directory reference
```

`fetch_fp32.py` は公開リリースのメタデータに対してソースとモデルのSHA256を照合します。
FP32は比較用で、本体に入れる認識モデルの変更ではありません。

## 画面と共有フォント

画面は最新の人間の指示に従い、Noto Sans JPを48pxで直接ラスタ化したVLWを使用します。
`assets/noto48.vlw` と `noto48-source.json` は画面用です。印刷のサイズは段3の計画を保ち、
同じ出典のNotoデータを使います。既存の64pxデータを画面へ縮小表示しているのではありません。
語彙82文字と現在のUI文言・数値表示に必要な文字を含みます。出典コミット・SHA256・
グリフ寸法は `assets/noto48-source.json` と `assets/noto48-metrics.json`、OFLは
`licenses/NotoSansJP-OFL.txt` に記録しています。

```sh
uv run --with pillow --with fonttools scripts/build_font.py --pixels 48
```

各行と可視行全体を中央配置し、行の上下端に応じた円の弦幅でUTF-8文字を折り返します。
中央の行は広く使い、上下の行は狭めます。固定文言は意味の切れ目で改行します。
認識結果は行ごとの空き幅の二乗和を最小にする位置へ配分し、折返した段落の最後が1文字だけに
なる分割を避けます。1文字の認識結果そのものはそのまま表示します。
長文は上下スワイプでスクロール。タップを離すと録音を開始し、スワイプでは開始しません。
診断用の `SHOT` は最終表示フレームバッファのRGB・行配置・元文字列中の範囲を出します。
CO5300パネルのガラスからの読取りは非対応です。OPI-PSRAMの実表示用フレームバッファを
読み、各文字のネイティブVLW画素と比較して、行途中の欠けも検査します。
`NEXT` / `PREV` は1行スクロール、`VMAX` は最大198文字の明示的な表示試験入力です。
これらは認識結果を偽装せず、印刷しません。

```sh
uv run --with pyserial --with pillow scripts/screen_proof.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/evidence/screen --pcm /absolute/path/to/microphone.pcm --seconds 120
uv run --with pyserial --with pillow scripts/screen_proof.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/evidence/maximum --maximum-text --seconds 1200
```

収集PNG・JSONを保存し、円外画素0、各行の弦幅への収まり、Noto48pxの全画素と文字の保持、
左右・上下中央、アンチエイリアス階調、スクロールで全文に到達できることを検査します。
文字列のラベルだけでは欠けを証明せず、フレームバッファとVLWの文字画素マスクを照合します。
最大長の入力は録音のフレーム数とデコーダの2文字トークン上限から導出しています。
この長文試験は1行ずつ移動する全24位置を取得します。各画面は行番号付きのPIXELS行で受信し、
行の欠落・順序・幅を検査します。1200秒はホスト側の観測上限で、製品の待ち時間ではありません。

## 段2の実機試験

ESP-IDF同梱NimBLEを使い、外部PSRAM割当・1台接続（接続とスキャンの2活動）に設定します。
既存の段1 sdkconfigを保全して設定を生成し直す場合は、`idf.py -D SDKCONFIG=/project/sdkconfig.stage2 build`
で別の生成ファイルを使います。通常の新規ビルドはsdkconfig.defaultsから同じ設定になります。
BLEIはモデル保持中の起動・停止だけの事前試験、BLEQは実プリンターを探してff03/ff01を購読し、
ff02へ状態照会を送信します。クレジットが無ければ送らず、未確認の応答を正常に置き換えません。
アクティブスキャンでGHLPを含む名前、サービス0x0A71、実機で確認した製造元データ
（0x5941、5e55092395fd）のいずれかを探し、接続後にFF00サービスを必須確認します。
この実機の広告には名前がなく、製造元データからの検出で実照会に成功しました。
終了時は接続を切り、nimble_port_stop/deinitとコントローラー停止を確認します。

BLETは実マイク認識→BLE状態照会→停止→再録音を10回行います。紙は使いません。
USB給電で10往復を実測済みです。内部最小空き16315〜16439バイト、各回の終了時最大連続空き
25600バイトで、最小値の継続低下はありませんでした。電池だけの10往復も実測済みで、
全回USB未接続、内部最小16355〜16411バイト、終了時最大連続38912バイト、継続低下なしでした。
各回で内部メモリの最小値を測り直し、前回から低下した次の回も低下したら停止・報告します。
各段階の空き・最大連続空き・最小空き・PSRAMと、USB接続・電池/VBUS電圧を記録します。
記録は明示PSRAMバッファに保持し、電池試験後にもBLEDで回収します。
BARMで電池試験を準備し、USBを外してAを押すと同じ10回を開始します。
USB接続中のAでは開始せず、再接続後の取得まで再起動しません。

```sh
uv run --with pyserial scripts/check_ble_cycles.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/usb --operation run --power usb --seconds 1200
uv run --with pyserial scripts/check_ble_cycles.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/battery-arm --operation arm --seconds 120
# 人間によるUSBを外す→A→試験完了→USBを戻す操作の後
uv run --with pyserial scripts/check_ble_cycles.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/battery --operation dump --power battery --seconds 120
```

## 印字

同じ固定版Noto64の認識用82字を1bit化して印字します（行高69ドット、ビットマップ34320バイト）。
96ドット幅・先頭白16行・末尾100行送りで、画面プレビューと送信には同じラスタを使います。
FF03のクレジットを待ち、MTU100を交渉して90バイトずつ30ms以上空けて送り、AAを待ちます。
「やまだたろー」の実認識を1枚印刷し、AA・全5410バイト送信と、人間の紙確認
「きれいに出た、欠け・向き問題なし」を確認済みです。82字網羅・最大長等の追加検査は未確認です。

次の命令は実際に紙を1枚使います。認識文字が指定値と異なれば印刷しません。

```sh
uv run --with pyserial scripts/print_label.py --port /dev/cu.usbmodem1101 \
  --pcm /absolute/path/to/microphone.pcm --expected-text やまだたろー \
  --output /absolute/path/to/print-evidence --seconds 120
```

失敗後の自動再送は行いません。送信後にAAを受け取れなければ、既に印刷されたか不明として扱います。

## うごきモード（KoeAct-JP）

[KoeAct-JP](https://huggingface.co/ayousanz/KoeAct-JP) v0.1.1 で、話した依頼を文字起こしを挟まずに
Action JSON（11種類のツール、1回に2つまで）にし、画面のキャラクターを動かします。
モデルはRalomi e13 stream40（`koeact` 区画）とデコーダ（`a2` 区画、メモリマップ）で、
ひらがなモードのRalomi stream100と同時にはPSRAMに載らないため、モードはNVSに保存して再起動で切り替えます。

- 切り替え: 待機画面を1.5秒長押しタッチ（どちらのモードでも）、またはシリアル `MODE`
- **常時ヒアリング**: ボタンなしで話しかけると動きます。マイクは15秒のリングバッファへ録り続け、
  部屋の雑音の大きさ（下がるときは速く、上がるときはゆっくり追う）より12 dB大きい音が30 ms続いたら話し始め、
  雑音+7 dB・その発話のピーク−25 dBのどちらも下回って0.6秒たったら話し終わりとして切り出します（最長8秒）。
  依頼でない会話（`p_act` 0.97未満）は画面を変えません。処理中に話した分は捨て、古い依頼が遅れて動くことはありません
- 左上のボタンを押しながら話す（最長8秒）こともできます。右上のボタンで顔・位置・ライトを初期状態に戻します
- 長時間動かしても劣化しないように: 聞いている間はフラッシュに書き込みません（NVSへの書き込みはモード切り替えの時だけ）。
  録音・切り出し・推論の大きなバッファは起動時に1回だけ確保し、推論の途中結果も確保済みの領域に追記します。
  1分ごとに `LISTEN_STAT` と `MEM listening`（内部RAMとPSRAMの空き）を出すので、空きが減り続けないことを確認できます
- 前処理はKoeAct-JPのPC版（`koeact.runtime` の clean）と同じ: 発話全体を −23 dBFS、先頭0.5秒・末尾0.3秒の無音
- `p_act` が0.97未満なら依頼ではないとして何もしません（KoeAct-JPの `config.json` の閾値）

| 依頼の例 | 画面の変化 |
|---|---|
| 「笑って」「悲しい顔をして」「びっくりして」「怒って」 | 表情（7種類） |
| 「顔を右に45度向けて」「上を見て」 | 目線（look）。turn は顔ごと動く |
| 「三回うなずいて」「首を横に二回振って」「おじぎして」 | うなずき・首ふり・おじぎのアニメーション |
| 「ライトを赤にして」「ライトを消して」 | 画面の縁のリングの色（LEDの代わり） |
| 「画面をもっと明るくして」「画面を暗くして」 | 実際の画面の明るさ（AMOLED） |
| 「音量を30にして」 | 音量ゲージ（スピーカーはマイクとI2Sを共有するため鳴らしません） |
| 「やっぱりライトは消さないで」 | 何もしない（否定は `[]`） |

### 家電操作（テスト）

KoeAct-JPのActionは学習済みの11ツールに固定なので、家電は同じモデルが出す「かな」（CTCの文字起こし）を
`main/home.cpp` の規則で読み、画面の家電パネル（テレビ・エアコン・でんき）に反映します。
実機の家電にはつながず、シリアルに `HOME_ACTION {"device":...,"command":...}` を出します（赤外線リモコン等の接続口）。
家電の言葉を含む発話はKoeActのActionより優先し、「〜ないで」の否定は何もしません。

| 言い方 | 動き |
|---|---|
| テレビ／エアコン／電気／あかり を つけて・消して・オンにして・オフにして | 電源 |
| （エアコンを）冷房／暖房／ドライ にして | 運転モード（電源も入る） |
| 温度を〈数字〉度にして（16〜31℃） | 設定温度 |
| （テレビの）音量を〈数字〉上げて・下げて | テレビの音量（数字なしはテレビと言ったときだけ±1） |
| （テレビを）地上波／BS／CS にして | 放送の切り替え |

数字はかなの数詞（にじゅーはち → 28）として読みます。BS・CSはかなでは崩れやすい（「りえす」「しんえーす」）ため、
「えす」「えーす」の直前の1〜2文字で見分けます。規則の単体テスト:
`c++ -std=c++17 -I main -I scripts/host_stubs scripts/test_home_parse.cpp main/home.cpp -o test_home && ./test_home`

シリアル診断: `RLMA` で1発話の生PCMを送ると切り出し後と同じ処理を通し、`KOEACT`・`KOEACT_JSON`・`KOEACT_KANA` を返します。
`LPCM` + uint32サンプル数 + PCM はマイクの代わりに常時ヒアリングへ流し込みます（0.5秒ごとの `LPCM_ACK` を待って次を送る）。
`LSTA` で統計とメモリを出します。`LFRM` で画面を取得できます。
`scripts/check_listen.py` は雑音に依頼を混ぜた連続音声を流し込み、切り出し数・PC版との一致・周ごとのメモリを記録します。

```sh
uv run --with pyserial --with pillow scripts/check_action.py --port /dev/cu.usbmodem1101 \
  --output /absolute/path/to/evidence --wav a.wav --label 笑って --expect '[{"name":"set_expression","arguments":{"expression":"happy"}}]'
```

macOSの `say -v Kyoko` で作ったWAV 17本（再生はしない）で、実機の出力はPC版KoeAct-JPと17/17一致、
録音終了から結果まで0.75〜1.59秒（中央値1.06秒）、待機中の空きは内部RAM 55KB・PSRAM 645KBでした。
実際の人の声とマイクでの精度は未確認です。

## キャラクターの表示と診断

起動時は選択中の1体を大きく表示します。左右スワイプで6体を循環し、ダブルタップまたは
「みんなあつまって」「みんな集合」で集合表示へ。集合中にキャラをタップするとその1体へ戻ります。
声による向き・表情・ジャンプ・遊んで・背景変更は両方の表示で共通です。
待ち受け中の1.5秒長押しは既存の切り替えを維持し、移動やタップと分離しています。
選択はRAMで保持し、切り替えによる再起動時の既存NVS commitにだけ保存を加えます。
図形の3体は1体表示では468pxの画面作業領域へ実寸で直接描画し、拡大するのはペットのドット絵だけです。

`sprites` は `0xE00000` / 2MB。区画表は `tts`（`0xD00000` / 1MB）も併記し、このアプリの書込定義は
`tts` のデータに触れません。素材の再生成は行わず、親が採用した追加画像を `scripts/clean_sprite_edges.py` →
`scripts/build_sprites.py` で処理します。原本と出所は `assets/pets/sources.json` に記録します。

`CTCH` 診断はヘッダーに続く11バイト（`<BhhhhH`）で種類、開始x/y、移動dx/dy、押下時間msを渡します。
種類は0=タップ、1=ダブルタップ、2=横スワイプ、3=長押し。実物と同じ判定器へ座標列を入れ、
表示を直接書き換える近道は使いません。長押しの診断は実際に再起動し、選択の保存も確認します。

```sh
python3 scripts/device_lock.py --wait 3600 -- uv run --with pyserial --with pillow \
  python scripts/check_character_touch.py --port /dev/cu.usbmodem1101 --output /absolute/path/to/evidence
c++ -std=c++17 -I main scripts/test_character_touch.cpp -o /tmp/stopwatch-touch && /tmp/stopwatch-touch
```

`LSTA` はメモリに加え `CHARACTER_STAT`（描画済みコマ数・経過時間・滑り量）と
`CHARACTER_VIEW`（集合表示か・選択中の番号）を返します。FPSは画像転送を挟まない2回のコマ数・時間の増分で計測します。

## 出典

Ralomi（ayousanz）esp32s3/firmwareを元にStopWatchへ移植。
Apache-2.0の `LICENSE`、`NOTICE`、`THIRD_PARTY_NOTICES.md` と `licenses/` を保持。
`UPSTREAM_SHA256SUMS` は移植前の配布ソース用で、変更後ソースのハッシュではありません。
`main/koeact/` はKoeAct-JP（ayutaz、Apache-2.0）v0.1.1 の `cores3/firmware/a2` をそのまま収録し、
`licenses/KoeAct-JP-LICENSE.txt`・`licenses/KoeAct-JP-NOTICE.txt` を保持しています。

## Spoken responses (sanoTTS-jp)

sanoTTS-jp v1.2.0 C99 inference is vendored unchanged in `components/sanotts/` (MIT).
The v4 int8 weights are **not MIT**: see `licenses/sanotts/LICENSE-MODEL.md` and
`licenses/sanotts/NOTICE.md` for attribution and mandatory output-use restrictions.
Apache-2.0 full text is included in `licenses/sanotts/LICENSE-APACHE-2.0.txt`.
Generated audio must not be distributed as reusable voice素材.

```text
This model was distilled from a piper-plus teacher model.
sanoTTS-jp — https://github.com/ayutaz/sanoTTS-jp

つくよみちゃんコーパス
  本ソフトウェアの音声合成には、フリー素材キャラクター「つくよみちゃん」
  （© 夢前黎）が無料公開している音声データを使用しています。
  https://tyc.rei-yumesaki.net/material/corpus/

教師 base の学習に使用した音声コーパス
（⚠️ 改変あり: いずれも音声合成モデルの学習に使用しています）:
  - LibriTTS-R (en) — Koizumi et al., 2023 — CC BY 4.0
      素材:       https://www.openslr.org/141/
      ライセンス: https://creativecommons.org/licenses/by/4.0/
  - CML-TTS (es / fr / pt) — freds0 et al. — CC BY 4.0
      素材:       https://github.com/freds0/CML-TTS-Dataset
      ライセンス: https://creativecommons.org/licenses/by/4.0/
  - AISHELL-3 (zh) — Shi et al., 2020 — Apache-2.0
      素材:       https://www.aishelltech.com/aishell_3
      ライセンス: https://www.apache.org/licenses/LICENSE-2.0
  上記 3 素材は現状のまま (AS IS) 提供され、明示・黙示を問わず保証はありません。

```

The `tts` partition occupies 0xD00000–0xDFFFFF only; 0xE00000–0xFFFFFF belongs to the shared `sprites` partition (data subtype 0x40).
The combined firmware retains both partition-table entries. Shared-device verification writes
only the partition table and application, preserving the existing TTS and sprite data.
Validate that write set with `scripts/verify_build.py --app-only --output <evidence.json>`.
No device kanji dictionary is included. Readings and accents are generated offline by
`scripts/generate_tts_phrases.py` using OpenJTalk, then stored in `main/tts_phrases.h`.
Numbers use composable digit/place-name pieces. Unsupported captions produce an explicit error.
All large synthesis buffers are allocated once at boot in PSRAM. Microphone capture stops
while speaking and discards the codec's existing one-second warmup before resuming.

For silent USB verification, `TMUT` enables PCM hex diagnostics and mutes only the hardware
speaker; `TLIV` restores normal spoken responses. This test preference survives mode reboots.
Each response emits `TTS_BEGIN`, `TTS_END`, `TTS_MEM`, and listener pause/restore logs.
`TTS_END` also records allocation-tracked `psram_min` and `internal_min` from
synthesis initialization through the completion of speaker playback. The preceding
monitoring interval is retained in `TTS_RAM_PREVIOUS`; subsequent `MEM` minimum
fields cover the interval since that response began, rather than all previous responses.
Use `scripts/tts_reference.cpp` with `SAAN_INT8_ACT=1 SAAN_PIE_EMU=1` to compare host PCM.
Always wrap device operations with `python3 scripts/device_lock.py --wait 3600 -- ...`.

あいさつ・雑談は `main/talk_commands.h` の決まった文言だけに返事をします。
家電・印刷・キャラ命令を先に判定し、文頭が一致して残りが3文字以内の短い発話を受け付けます。
否定文は除外します。選択中のキャラが喜ぶ表情になり、同じマイク停止・復帰経路で返事をします。

読みを再生成した場合は、PCの実際のsanoTTSコアで各文のピークを測ってからビルドします。
TTS区画のモデルは変更しません。モデルと読みを変えたときは倍率も作り直してください。

```sh
# generate_tts_phrases.py のOpenJTalk生成後
python3 scripts/build_tts_reference.py
python3 scripts/generate_tts_levels.py --reference build/host-tts/tts_reference --report build/tts-levels.json
python3 scripts/build_tts_reference.py
```

倍率は読み表に保存します。実行時は1回だけ合成し、数値をつなぐ文では部品の最小倍率を使います。
出力を-1dBFSに制限し、PCMのクリップを防ぎます。未計測の倍率があればビルドを止めます。
StopWatchのES8311 DACは、M5Unifiedの音量計算からクリップしない最大段階を選びます。
AW8737Aは既存の最大出力モードを維持し、既定音量は50%です。
全読み表の実機検査には、起動完了とTMUT応答の確認後に`TTBL`を送ります。
同じ発話経路で全項目のPCMと復帰ログを出し、最後に`TTS_TABLE_DONE`を出します。
消音を確認できないときは実行しません。終了時は音量50%とTLIV応答を必ず確認します。

## ローカルのペット（公開リポジトリには含めない）

`pets/<名前>/pet.json` と `pets/<名前>/spritesheet.webp` に Codex のペットフォルダをコピーします。
標準の1536×1872px、8列×9行を使います。0〜3体までで、追加の4行があれば同じフォルダの
`extra.png`（8列×4行）に置きます。ペットの権利は利用者が確認してください。
`pets/` と生成した `assets/pets/` はgit対象外です。生成前の画像・生成結果は送信しません。

ビルド前に `uv run --with pillow python scripts/build_sprites.py --report /tmp/botwatch-pets.json` を実行します。
既定の選択順はフォルダ名順です。`--pet pet-one --pet pet-two` のように指定すると、その順序になります。
0体なら `pets/` を空にして実行します。0体版は古いsprites区画を読まず、そこへ書き込みません。
1〜3体版では生成データの配置順とビルドで生成する体数が一致します。
以前の選択が新しい体数を超えたときは最初のキャラに戻ります。

追加画像が無いときだけ、次の明示的な対応で既存行を使います。追加画像がある場合はその画素を使います。

| 追加の動作 | 代用する既存行（0始まり） |
|---|---|
| 後ろ向き（9） | 待機（0） |
| 怒る（10） | 興奮・喜ぶ（3） |
| 上を見る（11） | 待機（0） |
| 下を見る（12） | 待機（0） |

## 印刷プリセットの設定

公開用の見本は `main/presets_local.example.h` にあります。
自分用はこのファイルを `main/presets_local.h` にコピーし、かな・画面表示名・印刷名の3列を変更します。
ローカル設定はgit対象外です。ビルドは選ばれた名前から印刷フォントも毎回生成し、
`main/print_font.inc` と `assets/print-font.json` もgit対象外にします。
見本を強制してビルドする場合は `idf.py -DBOTWATCH_USE_LOCAL_PRESETS=OFF build`、
ローカル設定へ戻す場合は `idf.py -DBOTWATCH_USE_LOCAL_PRESETS=ON build` を使います。
フォント元に無い漢字は変換時に明示的に失敗します。

実機試験は `scripts/device_lock.py` で排他し、起動完了後にTMUTの応答を確認してから音声を注入します。
終了時は失敗した場合もTLIVを送り、`TTS_DIAGNOSTIC muted=0 pcm=0` を確認して戻します。
