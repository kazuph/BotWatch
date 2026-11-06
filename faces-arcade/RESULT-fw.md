# CoreS3 ファームウェアの受け入れ証拠

## LED の安全対策（進行中）

- 点灯要求を `arcade_led` で受け、各色を 1 段ずつ変更する。段の間隔は 15,874 マイクロ秒以上、各色の上限は 64。更新が遅れても複数段を一度に進めない。
- `clang++ -std=c++17 -Wall -Wextra -Werror faces-arcade/tests/led_ramp_test.cpp -o /tmp/faces-led-ramp-test && /tmp/faces-led-ramp-test` は成功。急な目標の反転、長い遅延、上限、上昇と下降を検査した。
- 同試験は白の目標値が開始時 48、1.5 秒で 24、3 秒以降 0 となり、3 秒間増えないことも検査する。試験対象の関数を含む CoreS3 ビルド `j8524` は成功。実機に書き込んだ版は等価な式を使う `j8519`。
- `rg 'led_strip_(set_pixel|refresh|clear)|Color::blue|white_count' faces-arcade/src` で LED 出力は `arcade_led.cpp` のみ、青色要求と灯数切替は存在しないことを確認した。
- 親の決定に従い、起動時に一度消灯し、撮影時の白の目標値を 48 から 3 秒で 0 へ下げる。CoreS3 ビルド `j8466`、書き込みとハッシュ確認 `j8484` は成功。周波数アプリの LED は店長が目視で確認した。光の変化速度の実測と、撮影時の LED の見た目は未検証。

依頼書: `.artifacts/cores3-camera/goal-fw.md`。承認済み計画: `.artifacts/cores3-camera/plan.html`。未達は実行・検証が残る項目です。

| 受け入れ条件 | 状態 | 確認コマンド・画面ファイル・結果 |
|---|---|---|
| CoreS3 の ESP-IDF 5.5.5 ビルド | 達成 | `docker run --rm -v "$PWD/faces-arcade:/project" -w /project espressif/idf:v5.5.5 idf.py -B build-cores3 -D SDKCONFIG=sdkconfig.cores3 -D IDF_TARGET=esp32s3 build` が診断コード除去後の `j8592` で成功。 |
| Core2 の ESP-IDF 5.5.5 ビルド | 達成 | `docker run --rm -v "$PWD/faces-arcade:/project" -w /project espressif/idf:v5.5.5 idf.py -B build-core2 -D SDKCONFIG=sdkconfig.core2 -D IDF_TARGET=esp32 build` は画面転送の修正後の `j8593` で成功。 |
| CoreS3 への書き込み | 達成 | `uv run --with esptool==5.6.0 python -m esptool --chip esp32s3 -p /dev/cu.usbmodem11401 -b 460800 --before default_reset --after hard_reset write_flash @flash_args` が job `j8401` で成功し、書き込みハッシュを検証。NVS 領域は書き込み対象に含まない。起動と機能は別途確認中。 |
| メニュー・周波数の画面 | 一部達成 | 店長が実機のメニュー表示を確認。完全な電源入れ直し後の `j8527` と、書き込み `j8537` と esptool の再起動後に電源を入れ直さず実行した `j8541` の両方で、320×240・7 色の画面を取得。macOS Vision は両画像から「タップか上下+A」「音あて」「和音あて」「あかな読み」「周波数メーター」を読み取った。書き込みと再起動だけでは白くならなかった。周波数画面は再取得が必要。 |
| 顔検出・分類・文字・説明の各画面 | 一部達成 | `j8552` の `face-recapture.png` は 320×240・1,579 色で、macOS Vision が「顔の検出」を読み取った。`j8581` の `classify-final.png` は分類名3件と確率3件を読み取れ、各行と最下端の背景色が一致。文字・説明の画面は未取得。 |
| 保存後・写真閲覧・エラーの画面 | 一部達成 | `j8570` の `sd-error-fixed.png` から「SD を使えません」を文字認識。保存成功後と写真閲覧は未取得。 |
| SD の JPEG を PC でデコード | 達成 | `j8679` で `IMG_0004.JPG` を保存し、Mac で 640×480 の JPEG としてデコード。`j8666` は既存ファイル `IMG_0001.JPG` と `IMG_0002.JPG` を確認して `IMG_0003.JPG` を保存した。0001 の作成者は未確認。 |
| 撮影時の内部 RAM / PSRAM 空きと最大連続空き | 達成 | `j8679` の 640×480 撮影中は内部 169119 / 77824 バイト、PSRAM 5960964 / 5636096 バイト。撮影とプレビュー復帰は 0.750490 秒。 |
| 顔検出・分類時の内部 RAM / PSRAM 空きと最大連続空き | 達成 | `j8552` の顔検出時は内部 169459 / 77824 バイト、PSRAM 7660828 / 7602176 バイト。`j8557` の分類時は内部 152719 / 45056 バイト、PSRAM 2793364 / 2686976 バイト。 |
| 保存・送信時の内部 RAM / PSRAM 空きと最大連続空き | 一部達成 | `j8679` の保存完了後は内部 167435 / 77824 バイト、PSRAM 7492548 / 7340032 バイト。保存は 0.356867 秒。送信時は未実測。 |
| 顔検出・分類の実測秒数 | 達成 | `j8552` の顔検出は 0.223871 秒、続く例は 0.078053・0.088198 秒。`j8557` の分類は 2.303060 秒、続く例は 1.426432・1.433045 秒。 |
| 音あて・和音あて・かな読みの出入り | 達成 | `j8574` でシリアルのタッチ入力から各アプリを開き、SELECT 入力でメニューへ復帰した。音の出力やゲーム内容はこの試験の対象外。 |
| 周波数・ブロック・へび・ぱたぱたの出入り | 達成 | `j8574` でシリアルのタッチ入力から各アプリを開き、SELECT 入力でメニューへ復帰した。ゲーム内容はこの試験の対象外。 |
| スコアの再起動後保持 | 未達 | 従来の NVS 位置 0x9000、サイズ 0x5000 を保持する設定にした。実機の保持は未検証。 |
| かな音声・SELECT・タッチ | 未達 | 実機回帰未実施。 |
| 音あて→周波数→かな→カメラ→周波数 | 未達 | 実機回帰未実施。 |
| 日本語名1000件の順序・重複・空欄 | 達成 | `python3 faces-arcade/scripts/check_imagenet_labels.py` が PASS。ESP-DL 3.3.13 の分類順と件数、空欄、重複を検査。翻訳内容と画面表示は未検証。 |
| 実音 440Hz と LED の見た目 | 達成 | 14:33 に店長が実機で確認。スマホの 440Hz に対し画面は 440 前後と「ラ」を表示し、LED は緑だけでゆっくり明るくなり、音を止めるとゆっくり消えた。撮影カウントダウンの LED の見た目は未確認。 |
| 実物の印刷物による OCR | 未達 | 実機準備後に親へ人の操作を依頼する。 |
| Wi-Fi・Tailscale キー投入 | 達成 | 親の報告では店長が `faces-arcade/tools/provision.py` で投入し、「保存した」を確認。値は受け取っていない。`j8821` で Wi-Fi と tailnet の接続を確認。 |
| 文字よみの実機通信・結果・削除 | 未達 | `j8821` で Wi-Fi と tailnet への接続を確認。文字よみ POST は中止され、結果と DELETE は未確認。再試行 `j8825` では本体のシリアル接続が途中で消えた。 |
| 写真の説明の実機通信・結果 | 未達 | 文字よみと同じ接続経路の実機確認待ち。 |
| 通信中の SELECT による中止 | 一部達成 | `j8774` で文字よみ開始直後に SELECT を送り、通信処理が `cancelled=1` で終了してメニューへ戻った。サーバー送信中の中止と古い結果の抑止は未検証。 |

カメラの通常プレビューは `faces-arcade/src/camera.cpp:179` で映像をスプライトへ描き、`:221` 以降で見出し・顔枠・人数・分類結果・カウントダウンを同じスプライトに描き、`:259` の `pushSprite` で画面へ1回送る。通常フレーム内に `M5.Display` への直接描画はない。写真閲覧・保存結果・エラーは別画面であり、直接描画を使う。`j8666` の連続3画像 `preview-0.png`〜`preview-2.png` は見出し部分の画素が一致し、文字認識で全て「モード：顔の検出」を検出。静止画では画像間の点滅を測れず、実機目視は新しい UI の後に残す。

## 決定と確認済みの範囲

- 親の最新の決定に従い、LED は各色上限 64 とし、撮影時の白点灯は廃止した。白 48 の目標を 3 秒で消すカウントダウンと変化速度制限を実装し、ビルド済み。撮影時の見た目は未確認。
- 親から、説明先 `100.80.152.112:8193`、モデル `vision`、出力上限 128、指定の日本語プロンプトを受領し、コードに反映した。実機確認は未実施。
- 親の許可により MicroLink 本家 `216da3300f0493b0860247d43f7af5ce29df63a5` の TCP 接続と送信の局所パッチを追加。`python3 faces-arcade/scripts/prepare_microlink.py` は固定コミットとパッチを検証した。固定コミットの元ファイルにパッチを適用し、適用済みファイルと `cmp` で一致した。通信期限の実測は未実施。
- 文字認識 API の `GET http://100.80.152.112:8016/openapi.json` で `file`・`output_format`・`lite` とジョブ削除経路を確認。POST と削除の実行は未実施。
- `uv run --with pyserial --with pillow --with esptool==5.6.0 python -c 'import serial, PIL, esptool; ...'`（job `j8312`）でツールを準備。取得・設定スクリプトは `python3 -m py_compile` のみ実施。
- 土台は `8fa617c`、日本語名表は `9cdd182`、MicroLink は `2d79ef3` と `4305a27`、LED 制限は `6932763`、画面取得道具は `d6cfcff`、カメラ・通信の実装は `c6a31cd`。SD 保存は実機で確認済み。文字よみ・説明の通信は引き続き未達。

## 統合ビルドの再開

job `j8377` は IP アドレス変換関数のコンパイルエラー、`j8391` は暗号機能のリンクエラーで失敗した。修正後、`j8393` が成功した。実機への書き込み `j8401` と Core2 の統合ビルド `j8402` を開始した。

`j8401` で書き込んだ版は PSRAM の初期化に失敗し再起動を繰り返した。シリアル出力は `octal_psram: PSRAM chip is not connected, or wrong PSRAM line mode`。4 線設定に修正し、`j8409` でビルド成功、`j8417` で書き込み中。起動は未確認。

- 応答 JSON の部分検査: ESP-IDF 5.5.5 が指定する cJSON コミット `b2890c8d76bbb64e710585ebc0a917196b9c67e7` の実物を使い、`cc -c /tmp/botwatch-cjson/cJSON.c -o /tmp/botwatch-cjson/cJSON.o` と `clang++ -std=c++17 -I/tmp/botwatch-cjson -Ifaces-arcade/src faces-arcade/src/network_result.cpp /tmp/botwatch-cjson/check.cpp /tmp/botwatch-cjson/cJSON.o -o /tmp/botwatch-cjson/check`、`/tmp/botwatch-cjson/check` が成功。入力と判定は `.artifacts/cores3-camera/network-result-check.cpp`。正常・不正 JSON・空文字・不正 ID を検査。実機通信は未検証。

- HTTP 応答の部分検査: ESP-IDF 5.5.5 の実際の `components/http_parser/http_parser.c` を使い、`cc -c /tmp/botwatch-http-parser/http_parser.c -o /tmp/botwatch-http-parser/http_parser.o`、`clang++ -std=c++17 -I/tmp/botwatch-http-parser -Ifaces-arcade/src faces-arcade/src/http_response.cpp /tmp/botwatch-http-parser/check.cpp /tmp/botwatch-http-parser/http_parser.o -o /tmp/botwatch-http-parser/check`、`/tmp/botwatch-http-parser/check` が成功。入力は `.artifacts/cores3-camera/http-response-check.cpp`。1バイトずつの固定長・chunked・途中切断・不正 HTTP・503 を検査。実機通信は未検証。
- 接続の期限と文字・説明の期限を分けて数えるコード、および画面側が期限切れを表示するコードを追加した。通信ライブラリ内部の処理が戻るまで、期限後もしばらく裏のタスクが残る経路がある。開始直後の SELECT による復帰は確認済み。送信中の SELECT、残った処理の終了、30・60・180 秒の実測は未検証。
- ESP-IDF 5.5.5 の `components/esp_event/esp_event.c` を確認。イベント配信中はループの mutex を保持し、別タスクからの解除は mutex を取得してから行う。Wi-Fi ハンドラー解除後のイベントグループ破棄についてのソース確認であり、実機での中止・再送テストではない。
- ESP-IDF 5.5.5 の FAT VFS ソースでは、`fsync` は `f_sync`、`close` は `f_close` の結果を返す。保存処理が両方の失敗を判定する根拠を確認した。実機の SD 保存は別途確認済み。
- ESP-DL 3.3.13 の `Model::get_output("")` は単一出力を返し、分類後処理はその出力へ Softmax を適用して分類順に走査する。分類番号による日本語名の対応はソース上の確認であり、推論結果の実測ではない。

## 残りの確認と再開手順

残りの確認: 文字よみの POST が中止された理由、結果表示、DELETE、説明、送信中の SELECT 中止、古い結果の抑止、印刷物での受け入れ。

1. 店長が戻ったら親を通じて USB 接続を復旧し、シリアルポート名と本体の応答を確認する。認証情報は表示・保存しない。
2. CoreS3 用ビルドと書き込み後、文字よみで 640×480 の写真を撮り、Wi-Fi・tailnet 接続と 8016 への POST の経過を記録する。`j8821` の中止が期限切れか SELECT かを切り分ける。
3. 文字よみの応答を画面で確認し、ジョブの DELETE とサーバー側の削除を確かめる。送信中に SELECT で中止し、遅れて届いた古い結果が表示されないことも確かめる。
4. 説明モードで 8193 へ送信し、結果表示を確認する。文字よみが通ったら親に知らせ、印刷物での受け入れを店長に依頼する。
5. 実測結果と未確認の項目をこの表に追記する。通信中の watchdog 警告と RAM・スタックの空きもログで確認する。

`j8795` と `j8805` では既定イベントタスクのスタック不足で再起動。イベント内のログ整形を移し、CoreS3 のスタックを 4608 バイトに変更した `j8821` では tailnet 接続まで進んだ。送信中の動作は引き続き未確認。Core2 は 2304 バイトのまま。
