// M5Stack (ESP32) から 2523/GHLP_BLE ラベルプリンターへ文字ラベルを印刷するサンプル。
// プロトコルは docs/PROTOCOL.md。M5Stack StopWatch（ESP32-S3）の実機で印刷を確認済み（2026-10-24）。
//
// 依存: M5Unified, NimBLE-Arduino (2.x)
// ボタン A を押すと LABEL_TEXT を印刷する。

#include <M5Unified.h>
#include <NimBLEDevice.h>

static const char *LABEL_TEXT = "テスト BotWatch";
static const char *TARGET_NAME = "GHLP";

static NimBLEUUID UUID_SVC("0000ff00-0000-1000-8000-00805f9b34fb");
static NimBLEUUID UUID_RX("0000ff01-0000-1000-8000-00805f9b34fb");
static NimBLEUUID UUID_TX("0000ff02-0000-1000-8000-00805f9b34fb");
static NimBLEUUID UUID_CX("0000ff03-0000-1000-8000-00805f9b34fb");

static const int HEAD_DOTS = 96;   // 1 行のうち紙に出るのは先頭 96 ドット
static const int LEAD_ROWS = 16;   // 先頭が黒すぎると欠けるので 2mm 白を入れる
static const int CHUNK = 90;
static const int PACKET_DELAY_MS = 30;

static NimBLEClient *client;
static NimBLERemoteCharacteristic *txChar;
static volatile int credits = 0;
static volatile int lastReply = -1;

static void onCx(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  if (n == 2 && d[0] == 1) credits += d[1];
}
static void onRx(NimBLERemoteCharacteristic *, uint8_t *d, size_t n, bool) {
  if (n >= 1) lastReply = d[0];
}

// StopWatch の画面は丸いので中央に表示する
static void showStatus(const char *msg) {
  M5.Display.fillScreen(TFT_BLACK);
  M5.Display.setTextDatum(middle_center);
  M5.Display.drawString(msg, M5.Display.width() / 2, M5.Display.height() / 2);
}

static bool connectPrinter() {
  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setActiveScan(true);
  NimBLEScanResults res = scan->getResults(8000, false);
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice *dev = res.getDevice(i);
    if (dev->getName().find(TARGET_NAME) == std::string::npos) continue;
    client = NimBLEDevice::createClient();
    if (!client->connect(dev)) return false;
    NimBLERemoteService *svc = client->getService(UUID_SVC);
    if (!svc) return false;
    txChar = svc->getCharacteristic(UUID_TX);
    credits = 0;
    svc->getCharacteristic(UUID_CX)->subscribe(true, onCx);
    svc->getCharacteristic(UUID_RX)->subscribe(true, onRx);
    delay(300);
    if (credits == 0) credits = 4;
    return txChar != nullptr;
  }
  return false;
}

static void sendData(const uint8_t *data, size_t len) {
  for (size_t i = 0; i < len; i += CHUNK) {
    uint32_t t0 = millis();
    while (credits <= 0) {
      if (millis() - t0 > 2000) { credits = 1; break; }  // 純正アプリと同じ救済
      delay(1);
    }
    size_t n = min((size_t)CHUNK, len - i);
    txChar->writeValue(data + i, n, false);
    credits--;
    delay(PACKET_DELAY_MS);
  }
}

// 文字を横書きで描き、時計回りに 90° 回したラスタ（1 行 = 12 バイト）にする
static void printText(const char *text) {
  M5Canvas c(&M5.Display);
  c.setColorDepth(1);
  c.setFont(&fonts::efontJA_24);
  c.setTextSize(2);                      // 48px 高
  int w = c.textWidth(text) + 48;        // 前後 3mm
  c.createSprite(w, HEAD_DOTS);
  c.fillSprite(0);
  c.setTextColor(1);
  c.setTextDatum(middle_center);
  c.drawString(text, w / 2, HEAD_DOTS / 2);

  const int rowBytes = HEAD_DOTS / 8;
  const int rows = w + LEAD_ROWS;
  size_t rasterLen = (size_t)rowBytes * rows;
  size_t total = 15 + 4 + 8 + rasterLen + 3 + 4;
  uint8_t *job = (uint8_t *)calloc(total, 1);
  size_t p = 15;                                          // 00 x15
  const uint8_t enable[] = {0x10, 0xFF, 0xF1, 0x02};
  memcpy(job + p, enable, 4); p += 4;
  const uint8_t hdr[] = {0x1D, 0x76, 0x30, 0x00, (uint8_t)rowBytes, 0,
                         (uint8_t)(rows & 0xFF), (uint8_t)(rows >> 8)};
  memcpy(job + p, hdr, 8); p += 8;
  p += (size_t)rowBytes * LEAD_ROWS;                      // 白い行
  // 回転後の行 y = 元画像の列 x、行の中の列 = 元画像の下から上（時計回り 90°）
  for (int x = 0; x < w; x++) {
    for (int col = 0; col < HEAD_DOTS; col++) {
      int srcY = HEAD_DOTS - 1 - col;
      if (c.readPixel(x, srcY)) job[p + col / 8] |= 0x80 >> (col % 8);
    }
    p += rowBytes;
  }
  const uint8_t tail[] = {0x1B, 0x4A, 100, 0x10, 0xFF, 0xF1, 0x45};
  memcpy(job + p, tail, 7); p += 7;
  c.deleteSprite();

  lastReply = -1;
  sendData(job, p);
  free(job);
  uint32_t t0 = millis();
  while (lastReply < 0 && millis() - t0 < 15000) delay(10);
  showStatus(lastReply == 0xAA ? "印刷OK" : "応答なし/エラー");  // AA = 成功
}

void setup() {
  M5.begin();
  M5.Display.setFont(&fonts::efontJA_24);
  NimBLEDevice::init("");
  showStatus("接続中…");
  showStatus(connectPrinter() ? "接続OK タップで印刷" : "プリンターが見つからない");
}

void loop() {
  M5.update();
  bool tapped = M5.Touch.getCount() > 0 && M5.Touch.getDetail().wasClicked();
  if ((M5.BtnA.wasPressed() || tapped) && client && client->isConnected()) {
    showStatus("印刷中…");
    printText(LABEL_TEXT);
  }
  delay(10);
}
