// Side-scrolling flapping game written for FACES ARCADE (own graphics).
#include "common.h"

namespace {
const int kW = 320, kH = 240, kGround = 28, kPlayH = kH - kGround;
const int kBirdX = 84, kPipeW = 38, kGap = 82, kSpacing = 158, kPipes = 3;
const float kGravity = 0.21f, kFlap = -3.7f, kScroll = 1.7f;
const uint32_t kStepMs = 16;
const char* const kKey = "flappy";

enum State { TITLE, READY, PLAY, FALL, OVER };
State state;
M5Canvas frame(&M5.Display);
float birdY, birdV, pipeX[kPipes], scrollX;
int pipeGap[kPipes];
bool pipeScored[kPipes];
int score, wing;
uint32_t lastStep, overAt, frames;

const uint16_t kSky[6] = {rgb(70, 170, 235), rgb(90, 185, 240), rgb(115, 200, 243),
                          rgb(145, 215, 245), rgb(175, 228, 245), rgb(205, 238, 240)};

int randomGap() { return random(36, kPlayH - kGap - 30); }

void resetRound() {
    birdY = kPlayH / 2 - 10;
    birdV = 0;
    score = 0;
    scrollX = 0;
    for (int i = 0; i < kPipes; ++i) {
        pipeX[i]      = kW + 70 + i * kSpacing;
        pipeGap[i]    = randomGap();
        pipeScored[i] = false;
    }
}

void drawCloud(int x, int y, int s) {
    frame.fillCircle(x, y, s, TFT_WHITE);
    frame.fillCircle(x + s, y - s / 2, s + 2, TFT_WHITE);
    frame.fillCircle(x + s * 2 + 2, y, s, TFT_WHITE);
    frame.fillRect(x - s, y, s * 3 + 2 + s, s, TFT_WHITE);
}

void drawPipe(int x, int gapY) {
    const uint16_t body = rgb(90, 190, 70), light = rgb(160, 235, 120), dark = rgb(50, 130, 45), edge = rgb(30, 80, 30);
    auto segment = [&](int y, int h) {
        if (h <= 0) return;
        frame.fillRect(x + 3, y, kPipeW - 6, h, body);
        frame.fillRect(x + 7, y, 6, h, light);
        frame.fillRect(x + kPipeW - 11, y, 6, h, dark);
        frame.drawFastVLine(x + 3, y, h, edge);
        frame.drawFastVLine(x + kPipeW - 4, y, h, edge);
    };
    auto cap = [&](int y) {
        frame.fillRect(x, y, kPipeW, 12, body);
        frame.fillRect(x + 4, y + 1, 7, 10, light);
        frame.fillRect(x + kPipeW - 9, y + 1, 7, 10, dark);
        frame.drawRect(x, y, kPipeW, 12, edge);
    };
    segment(0, gapY - 12);
    cap(gapY - 12);
    const int lower = gapY + kGap;
    cap(lower);
    segment(lower + 12, kPlayH - lower - 12);
}

void drawBird(int cx, int cy) {
    const uint16_t edge = rgb(70, 45, 20);
    frame.fillEllipse(cx, cy, 13, 10, edge);
    frame.fillEllipse(cx, cy, 11, 8, rgb(255, 215, 60));
    frame.fillEllipse(cx - 1, cy + 4, 8, 4, rgb(255, 240, 170));
    // Eye and beak.
    frame.fillCircle(cx + 5, cy - 4, 4, edge);
    frame.fillCircle(cx + 5, cy - 4, 3, TFT_WHITE);
    frame.fillCircle(cx + 6, cy - 4, 1, TFT_BLACK);
    frame.fillRoundRect(cx + 8, cy - 1, 9, 4, 2, edge);
    frame.fillRoundRect(cx + 9, cy, 7, 2, 1, rgb(240, 110, 50));
    frame.fillRoundRect(cx + 8, cy + 3, 7, 3, 1, edge);
    // Wing: position follows the flap animation.
    const int wy = cy + (wing == 0 ? -3 : wing == 1 ? 0 : 3);
    frame.fillEllipse(cx - 7, wy, 7, 4, edge);
    frame.fillEllipse(cx - 7, wy, 5, 3, rgb(255, 245, 200));
}

void render() {
    for (int i = 0; i < 6; ++i) frame.fillRect(0, i * (kPlayH / 6), kW, kPlayH / 6 + 1, kSky[i]);
    frame.fillRect(0, kPlayH - 4, kW, 4, kSky[5]);
    const int off = int(scrollX * 0.25f) % (kW + 90);
    drawCloud(60 - off + (off > 150 ? kW + 90 : 0), 40, 11);
    drawCloud(230 - off + (off > 300 ? kW + 90 : 0), 70, 9);
    // Distant hills.
    const int hillOff = int(scrollX * 0.5f) % 120;
    for (int i = -1; i < 4; ++i) frame.fillCircle(i * 120 - hillOff + 40, kPlayH + 10, 44, rgb(150, 215, 170));

    for (int i = 0; i < kPipes; ++i)
        if (pipeX[i] < kW && pipeX[i] > -kPipeW) drawPipe(int(pipeX[i]), pipeGap[i]);

    // Ground with scrolling stripes.
    frame.fillRect(0, kPlayH, kW, kGround, rgb(222, 205, 140));
    frame.fillRect(0, kPlayH, kW, 6, rgb(120, 200, 80));
    frame.drawFastHLine(0, kPlayH, kW, rgb(60, 110, 50));
    const int groundOff = int(scrollX) % 16;
    for (int x = -16; x < kW + 16; x += 16) frame.fillRect(x - groundOff, kPlayH + 1, 8, 5, rgb(155, 225, 100));
    frame.drawFastHLine(0, kPlayH + 6, kW, rgb(170, 150, 90));

    drawBird(kBirdX, int(birdY));

    if (state == PLAY || state == FALL) {
        char b[8];
        snprintf(b, sizeof(b), "%d", score);
        text(frame, b, kW / 2 + 2, 10, rgb(30, 50, 60), &fonts::FreeSansBold24pt7b, top_center);
        text(frame, b, kW / 2, 8, TFT_WHITE, &fonts::FreeSansBold24pt7b, top_center);
    }
    if (state == READY) {
        text(frame, "Aではばたく", kW / 2 + 1, 151, rgb(30, 50, 60), F32, top_center);
        text(frame, "Aではばたく", kW / 2, 150, TFT_WHITE, F32, top_center);
    }
    frame.pushSprite(0, 0);
}

void gameOver() {
    state  = OVER;
    overAt = millis();
    const bool best = submitScore(kKey, score);
    char b[32];
    snprintf(b, sizeof(b), "%d点 最高%d", score, bestScore(kKey));
    dialog("ゲームオーバー", pal::kRed, b, best ? "さいこう更新！" : nullptr, "A:もう一度");
    Serial.printf("FLAPPY gameover score=%d\n", score);
}

void stepPlay() {
    if ((pad_just & (kA | kB | kUp)) || tap_just) {
        birdV = kFlap;
    }
    birdV += kGravity;
    birdY += birdV;
    if (birdY < 10) { birdY = 10; birdV = 0; }
    scrollX += kScroll;
    for (int i = 0; i < kPipes; ++i) {
        pipeX[i] -= kScroll;
        if (pipeX[i] < -kPipeW) {
            float maxX = 0;
            for (float x : pipeX) maxX = max(maxX, x);
            pipeX[i]      = maxX + kSpacing;
            pipeGap[i]    = randomGap();
            pipeScored[i] = false;
        }
        // Forgiving hitbox: slightly smaller than the sprite.
        const int bx0 = kBirdX - 10, bx1 = kBirdX + 11, by0 = int(birdY) - 8, by1 = int(birdY) + 8;
        if (bx1 > pipeX[i] + 2 && bx0 < pipeX[i] + kPipeW - 2 && (by0 < pipeGap[i] - 2 || by1 > pipeGap[i] + kGap + 2)) {
            state = FALL;
            birdV = -1.0f;
            Serial.println("FLAPPY hit pipe");
        }
        if (!pipeScored[i] && pipeX[i] + kPipeW < bx0) {
            pipeScored[i] = true;
            ++score;
            Serial.printf("FLAPPY score=%d\n", score);
        }
    }
    if (int(birdY) + 9 >= kPlayH) {
        birdY = kPlayH - 9;
        state = FALL;
        Serial.println("FLAPPY hit ground");
    }
}

void enter() {
    if (!frame.getBuffer()) {
        frame.setColorDepth(16);
        frame.setPsram(true);
        if (!frame.createSprite(kW, kH)) {
            Serial.println("FLAPPY sprite=FAIL");
            text(M5.Display, "メモリ不足", 160, 100, pal::kRed, F32, top_center);
            state = TITLE;
            return;
        }
    }
    resetRound();
    state = TITLE;
    wing  = 0;
    render();
    text(M5.Display, "ぱたぱた", 160, 10, TFT_WHITE, F40, top_center);
    char b[20];
    snprintf(b, sizeof(b), "さいこう %d", bestScore(kKey));
    text(M5.Display, b, 160, 134, rgb(20, 40, 90), F24, top_center);
    text(M5.Display, "A:はじめる", 160, 160, rgb(20, 40, 90), F32, top_center);
    text(M5.Display, "SELECT:もどる", 160, 212, rgb(70, 60, 30), F24, top_center);
}

void tick() {
    if (state == OVER) {
        if (((pad_just & (kA | kStart | kB)) || tap_just) && millis() - overAt > 500) {
            resetRound();
            state = READY;
            Serial.println("FLAPPY ready");
        }
        if (state == OVER) return;
    }
    if (state == TITLE) {
        if ((pad_just & (kA | kStart | kB)) || tap_just) {
            state = READY;
            Serial.println("FLAPPY ready");
        }
        if (state == TITLE) return;
    }
    const uint32_t now = millis();
    if (now - lastStep < kStepMs) return;
    int steps = min<uint32_t>((now - lastStep) / kStepMs, 4);
    lastStep  = now;
    while (steps-- > 0) {
        ++frames;
        if (frames % 6 == 0) wing = (wing + 1) % 3;
        if (state == READY) {
            birdY = kPlayH / 2 - 10 + sin(frames * 0.08f) * 6;
            scrollX += kScroll;
            if ((pad_just & (kA | kB | kUp)) || tap_just) {
                state = PLAY;
                birdV = kFlap;
                Serial.println("FLAPPY start");
            }
        } else if (state == PLAY) {
            stepPlay();
        } else if (state == FALL) {
            birdV += kGravity * 1.3f;
            birdY += birdV;
            if (int(birdY) + 9 >= kPlayH) {
                birdY = kPlayH - 9;
                render();
                gameOver();
                return;
            }
        }
        pad_just = 0;  // one press acts once even if several physics steps run.
    }
    render();
}

void icon(int x, int y) {
    auto& d = M5.Display;
    d.fillRoundRect(x, y, 40, 40, 6, kSky[1]);
    d.fillRect(x + 28, y + 2, 9, 11, rgb(90, 190, 70));
    d.fillRect(x + 28, y + 26, 9, 12, rgb(90, 190, 70));
    d.fillEllipse(x + 15, y + 20, 9, 7, rgb(255, 215, 60));
    d.fillCircle(x + 19, y + 17, 3, TFT_WHITE);
    d.fillCircle(x + 20, y + 17, 1, TFT_BLACK);
    d.fillRect(x + 23, y + 20, 5, 3, rgb(240, 110, 50));
    d.fillEllipse(x + 10, y + 21, 4, 3, rgb(255, 245, 200));
}
}  // namespace

const Game kFlappy = {"ぱたぱた", kKey, enter, tick, icon, nullptr};
